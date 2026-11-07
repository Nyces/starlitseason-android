#include "ue4package.h"

#include <cstring>
#include <utility>

namespace liveview {
namespace {

constexpr uint32_t kAssetMagic = 0x9E2A83C1;

bool Fits(size_t pos, size_t need, size_t size) {
    return pos + need <= size;
}

int32_t ReadI32(const std::vector<uint8_t>& data, size_t pos) {
    int32_t value = 0;
    std::memcpy(&value, data.data() + pos, 4);
    return value;
}

int64_t ReadI64(const std::vector<uint8_t>& data, size_t pos) {
    int64_t value = 0;
    std::memcpy(&value, data.data() + pos, 8);
    return value;
}

void ReadFName(const std::vector<uint8_t>& data, size_t pos, int32_t* index, int32_t* number) {
    *index = ReadI32(data, pos);
    *number = ReadI32(data, pos + 4);
}

// FString：正长度单字节（latin1），负长度 UTF-16LE（长度取反，含结尾 0）。
bool ReadFString(const std::vector<uint8_t>& data, size_t* pos, std::string* out) {
    if (!Fits(*pos, 4, data.size())) {
        return false;
    }
    const int32_t length = ReadI32(data, *pos);
    *pos += 4;
    if (length == 0) {
        out->clear();
        return true;
    }
    if (length < 0) {
        const size_t count = static_cast<size_t>(-length);
        if (!Fits(*pos, count * 2, data.size())) {
            return false;
        }
        out->clear();
        for (size_t i = 0; i < count; ++i) {
            const uint16_t ch = static_cast<uint16_t>(data[*pos + i * 2] |
                                                     (data[*pos + i * 2 + 1] << 8));
            if (ch == 0) {
                break;
            }
            out->push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        *pos += count * 2;
        return true;
    }
    const size_t count = static_cast<size_t>(length);
    if (!Fits(*pos, count, data.size())) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(data.data() + *pos), count);
    while (!out->empty() && out->back() == '\0') {
        out->pop_back();
    }
    *pos += count;
    return true;
}

}  // namespace

std::string NameAt(const std::vector<std::string>& names, int32_t index) {
    if (index >= 0 && static_cast<size_t>(index) < names.size()) {
        return names[static_cast<size_t>(index)];
    }
    return "<name " + std::to_string(index) + ">";
}

bool ParsePackage(std::vector<uint8_t> uasset, std::vector<uint8_t> uexp, PackageData* out,
                  std::string* error) {
    const std::vector<uint8_t>& data = uasset;
    if (data.size() < 64) {
        *error = "uasset 过短";
        return false;
    }
    if (ReadI32(data, 0) != static_cast<int32_t>(kAssetMagic)) {
        *error = "包魔数不匹配";
        return false;
    }

    size_t pos = 8;
    if (ReadI32(data, 4) != -4) {
        pos += 4;
    }
    if (!Fits(pos, 16, data.size())) {
        *error = "包头越界";
        return false;
    }
    pos += 8;  // FileVersionUE4 / LicenseeVersion
    const int32_t custom_count = ReadI32(data, pos);
    pos += 4;
    if (custom_count < 0 || !Fits(pos, static_cast<size_t>(custom_count) * 20 + 4, data.size())) {
        *error = "自定义版本表越界";
        return false;
    }
    pos += static_cast<size_t>(custom_count) * 20;

    out->header_size = ReadI32(data, pos);
    pos += 4;
    if (!ReadFString(data, &pos, &out->package_name)) {
        *error = "包名解析失败";
        return false;
    }
    if (!Fits(pos, 4 + 40, data.size())) {
        *error = "包头尾部越界";
        return false;
    }
    pos += 4;  // PackageFlags
    const int32_t name_count = ReadI32(data, pos);
    const int32_t name_offset = ReadI32(data, pos + 4);
    const int32_t export_count = ReadI32(data, pos + 16);
    const int32_t export_offset = ReadI32(data, pos + 20);
    const int32_t import_count = ReadI32(data, pos + 24);
    const int32_t import_offset = ReadI32(data, pos + 28);
    pos += 32;
    const int32_t depends_offset = ReadI32(data, pos);  // 导出表步长的上界

    if (name_count < 0 || export_count < 0 || import_count < 0) {
        *error = "包表计数为负";
        return false;
    }

    // 名称表：FString + 4 字节 cooked 哈希
    out->names.clear();
    out->names.reserve(static_cast<size_t>(name_count));
    size_t q = static_cast<size_t>(name_offset);
    for (int32_t i = 0; i < name_count; ++i) {
        std::string name;
        if (!ReadFString(data, &q, &name) || !Fits(q, 4, data.size())) {
            *error = "名称表解析失败";
            return false;
        }
        q += 4;
        out->names.push_back(std::move(name));
    }

    // 导入表：28 字节定长
    out->imports.clear();
    out->imports.reserve(static_cast<size_t>(import_count));
    size_t p = static_cast<size_t>(import_offset);
    for (int32_t i = 0; i < import_count; ++i) {
        if (!Fits(p, 28, data.size())) {
            *error = "导入表越界";
            return false;
        }
        ImportObject item;
        item.class_package = NameAt(out->names, ReadI32(data, p));
        item.class_name = NameAt(out->names, ReadI32(data, p + 8));
        item.outer_index = ReadI32(data, p + 16);
        item.object_name = NameAt(out->names, ReadI32(data, p + 20));
        p += 28;
        out->imports.push_back(std::move(item));
    }

    // 导出表步长 = (depends_offset - export_offset) / export_count
    const int32_t stride = export_count > 0 ? (depends_offset - export_offset) / export_count : 0;
    out->exports.clear();
    out->exports.reserve(static_cast<size_t>(export_count));
    size_t e = static_cast<size_t>(export_offset);
    for (int32_t i = 0; i < export_count; ++i) {
        if (!Fits(e, 44, data.size())) {
            *error = "导出表越界";
            return false;
        }
        ExportObject object;
        object.class_index = ReadI32(data, e);
        object.outer_index = ReadI32(data, e + 12);
        object.name = NameAt(out->names, ReadI32(data, e + 16));
        // 注意：SerialSize / SerialOffset 是 int64（Python 侧读的是 "<qq"）
        object.serial_size = ReadI64(data, e + 28);
        object.serial_offset = ReadI64(data, e + 36);
        out->exports.push_back(std::move(object));
        e += static_cast<size_t>(stride > 0 ? stride : 44);
    }
    // 类名解析放在导出表读完之后，避免向前引用还没解析的导出项
    for (ExportObject& object : out->exports) {
        if (object.class_index < 0) {
            const size_t index = static_cast<size_t>(-object.class_index - 1);
            object.class_name = index < out->imports.size() ? out->imports[index].object_name
                                                            : "<import>";
        } else if (static_cast<size_t>(object.class_index) < out->exports.size()) {
            object.class_name = out->exports[static_cast<size_t>(object.class_index)].name;
        } else {
            object.class_name = "<null>";
        }
    }

    out->uasset = std::move(uasset);
    out->uexp = std::move(uexp);
    return true;
}

bool ExportBytes(const PackageData& package, const ExportObject& object,
                 std::vector<uint8_t>* out, std::string* error) {
    const int64_t offset = object.serial_offset - package.header_size;
    if (offset < 0 || offset + object.serial_size > static_cast<int64_t>(package.uexp.size())) {
        *error = "导出越界: " + object.name;
        return false;
    }
    out->assign(package.uexp.begin() + offset, package.uexp.begin() + offset + object.serial_size);
    return true;
}

bool ParsePropertyTag(const std::vector<uint8_t>& data, const std::vector<std::string>& names,
                      size_t pos, PropertyTag* out, size_t* next, bool* is_none,
                      std::string* error) {
    *is_none = false;
    if (!Fits(pos, 8, data.size())) {
        *error = "属性标签越界";
        return false;
    }
    int32_t name_index = 0;
    int32_t name_number = 0;
    ReadFName(data, pos, &name_index, &name_number);
    const std::string name = NameAt(names, name_index);
    if (name == "None" && name_number == 0) {
        *is_none = true;
        *next = pos + 8;
        return true;
    }
    if (!Fits(pos, 24, data.size())) {
        *error = "属性标签头越界";
        return false;
    }
    int32_t type_index = 0;
    int32_t type_number = 0;
    ReadFName(data, pos + 8, &type_index, &type_number);
    (void)type_number;
    const std::string type = NameAt(names, type_index);
    const int32_t size = ReadI32(data, pos + 16);
    const int32_t array_index = ReadI32(data, pos + 20);

    size_t p = pos + 24;
    std::string struct_name;
    std::string inner_type;
    if (type == "StructProperty") {
        if (!Fits(p, 8 + 16, data.size())) {
            *error = "结构体标签越界";
            return false;
        }
        struct_name = NameAt(names, ReadI32(data, p));
        p += 8 + 16;  // StructName + StructGuid
    } else if (type == "BoolProperty") {
        if (!Fits(p, 1, data.size())) {
            *error = "布尔标签越界";
            return false;
        }
        p += 1;
    } else if (type == "ByteProperty" || type == "EnumProperty") {
        if (!Fits(p, 8, data.size())) {
            *error = "字节标签越界";
            return false;
        }
        p += 8;
    } else if (type == "ArrayProperty" || type == "SetProperty" || type == "OptionalProperty") {
        if (!Fits(p, 8, data.size())) {
            *error = "数组标签越界";
            return false;
        }
        inner_type = NameAt(names, ReadI32(data, p));
        p += 8;
    } else if (type == "MapProperty") {
        if (!Fits(p, 16, data.size())) {
            *error = "映射标签越界";
            return false;
        }
        p += 16;
    }
    if (!Fits(p, 1, data.size())) {
        *error = "属性 GUID 标记越界";
        return false;
    }
    p += data[p] != 0 ? 1 + 16 : 1;
    if (size < 0 || !Fits(p, static_cast<size_t>(size), data.size())) {
        *error = "属性 " + name + " 的值越界";
        return false;
    }

    out->name = name;
    out->type = type;
    out->size = size;
    out->array_index = array_index;
    out->offset = p;
    out->struct_name = struct_name;
    out->inner_type = inner_type;
    *next = p + static_cast<size_t>(size);
    return true;
}

std::vector<PropertyTag> ParseProperties(const std::vector<uint8_t>& data,
                                        const std::vector<std::string>& names, size_t start) {
    std::vector<PropertyTag> result;
    size_t pos = start;
    std::string error;
    for (int i = 0; i < 65536; ++i) {
        PropertyTag tag;
        bool is_none = false;
        size_t next = pos;
        if (!ParsePropertyTag(data, names, pos, &tag, &next, &is_none, &error)) {
            break;
        }
        if (is_none) {
            break;
        }
        result.push_back(std::move(tag));
        pos = next;
    }
    return result;
}

const ExportObject* FindExport(const PackageData& package, const std::string& class_name) {
    for (const ExportObject& object : package.exports) {
        if (object.class_name == class_name) {
            return &object;
        }
    }
    return nullptr;
}

}  // namespace liveview
