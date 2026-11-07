#include "commands.h"

#include <cctype>
#include <cstring>
#include <cwctype>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ue4package.h"

namespace liveview {
namespace {

std::string LowerAscii(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string Num(int64_t value) {
    return std::to_string(value);
}

}  // namespace

std::string NarrowAscii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}

std::vector<std::wstring> FindPaks(const std::wstring& directory) {
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW((directory + L"\\*.pak").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return paths;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::wstring name = data.cFileName;
        std::wstring lower = name;
        for (wchar_t& ch : lower) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        // 与 Python 侧一致：默认跳过 add/free/lyric/song 这几类扩展包
        if (lower.rfind(L"add", 0) == 0 || lower.rfind(L"free", 0) == 0 ||
            lower.rfind(L"lyric", 0) == 0 || lower.rfind(L"song", 0) == 0) {
            continue;
        }
        paths.push_back(directory + L"\\" + name);
    } while (FindNextFileW(handle, &data) != 0);
    FindClose(handle);
    return paths;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(data.size());
    const bool ok = WriteFile(handle, data.data(), size, &written, nullptr) != 0 &&
                    written == size;
    CloseHandle(handle);
    return ok;
}

bool LoadAllPaks(const std::wstring& directory, const std::vector<uint8_t>& key,
                 std::vector<PakInfo>* out, const LogSink& log, std::string* error) {
    const std::vector<std::wstring> paths = FindPaks(directory);
    if (paths.empty()) {
        *error = "目录下没有找到 .pak 文件";
        return false;
    }
    for (const std::wstring& path : paths) {
        PakInfo info;
        std::string pak_error;
        if (LoadPakIndex(path, key, &info, &pak_error)) {
            log(info.path + "  version=" + Num(info.version) + "  entries=" +
                Num(static_cast<int64_t>(info.entries.size())) + "  mount=" + info.mount_point);
            out->push_back(std::move(info));
        } else {
            log("[FAIL] " + NarrowAscii(path) + ": " + pak_error);
        }
    }
    if (out->empty()) {
        *error = "所有 pak 索引都解析失败（密钥可能不对）";
        return false;
    }
    return true;
}

void SummarizePaks(const std::vector<PakInfo>& paks, const LogSink& log) {
    int64_t total = 0;
    for (const PakInfo& pak : paks) {
        total += static_cast<int64_t>(pak.entries.size());
    }
    log("共 " + Num(static_cast<int64_t>(paks.size())) + " 个 pak 索引解析成功，条目合计 " +
        Num(total) + " 条");
}

bool FindEntryAcrossPaks(const std::vector<PakInfo>& paks, const std::string& name_or_suffix,
                         const PakInfo** pak_out, const PakEntry** entry_out) {
    const std::string lower = LowerAscii(name_or_suffix);
    if (lower.empty()) {
        return false;
    }
    for (const PakInfo& pak : paks) {
        for (const PakEntry& entry : pak.entries) {
            const std::string name = LowerAscii(entry.name);
            if (name.size() >= lower.size() &&
                name.compare(name.size() - lower.size(), lower.size(), lower) == 0) {
                *pak_out = &pak;
                *entry_out = &entry;
                return true;
            }
        }
    }
    return false;
}

int64_t ListEntries(const std::vector<PakInfo>& paks, const std::string& filter, size_t limit,
                    const LogSink& log) {
    const std::string needle = LowerAscii(filter);
    int64_t total = 0;
    int64_t matched = 0;
    for (const PakInfo& pak : paks) {
        for (const PakEntry& entry : pak.entries) {
            ++total;
            if (!needle.empty() && LowerAscii(entry.name).find(needle) == std::string::npos) {
                continue;
            }
            ++matched;
            if (limit == 0 || static_cast<size_t>(matched) <= limit) {
                log(entry.name + "  usize=" + Num(entry.usize) +
                    "  method=" + Num(entry.method));
            }
        }
    }
    if (limit != 0 && static_cast<size_t>(matched) > limit) {
        log("（只显示前 " + Num(static_cast<int64_t>(limit)) + " 条）");
    }
    log("共 " + Num(total) + " 条，匹配 " + Num(matched) + " 条");
    return matched;
}

bool ExtractEntry(const std::vector<PakInfo>& paks, const std::vector<uint8_t>& key,
                  const std::string& name, const std::wstring& output, const LogSink& log,
                  std::string* error) {
    const PakInfo* pak = nullptr;
    const PakEntry* entry = nullptr;
    if (!FindEntryAcrossPaks(paks, name, &pak, &entry)) {
        *error = "找不到条目: " + name;
        return false;
    }
    std::vector<uint8_t> data;
    if (!ReadEntry(*pak, *entry, key, &data, error)) {
        return false;
    }
    if (!WriteFileBytes(output, data)) {
        *error = "写入文件失败: " + NarrowAscii(output);
        return false;
    }
    log(entry->name + " -> " + NarrowAscii(output) + "  (" + Num(static_cast<int64_t>(data.size())) +
        " 字节, usize=" + Num(entry->usize) + ")");
    return true;
}

bool DumpAsset(const std::vector<PakInfo>& paks, const std::vector<uint8_t>& key,
               const std::string& asset, const LogSink& log, std::string* error) {
    // 资产路径形如 Model/Character/.../S_chr_body_base；先拿 .uasset 再找同名 .uexp
    std::string stem = asset;
    const std::string lower_stem = LowerAscii(stem);
    if (lower_stem.size() > 7 && lower_stem.compare(lower_stem.size() - 7, 7, ".uasset") == 0) {
        stem = stem.substr(0, stem.size() - 7);
    }

    const PakInfo* uasset_pak = nullptr;
    const PakEntry* uasset_entry = nullptr;
    if (!FindEntryAcrossPaks(paks, stem + ".uasset", &uasset_pak, &uasset_entry)) {
        *error = "找不到资产: " + stem + ".uasset";
        return false;
    }

    std::vector<uint8_t> uasset;
    if (!ReadEntry(*uasset_pak, *uasset_entry, key, &uasset, error)) {
        *error = "读取 uasset 失败: " + *error;
        return false;
    }

    std::vector<uint8_t> uexp;
    const PakInfo* uexp_pak = nullptr;
    const PakEntry* uexp_entry = nullptr;
    if (FindEntryAcrossPaks(paks, stem + ".uexp", &uexp_pak, &uexp_entry)) {
        if (!ReadEntry(*uexp_pak, *uexp_entry, key, &uexp, error)) {
            *error = "读取 uexp 失败: " + *error;
            return false;
        }
    }

    PackageData package;
    if (!ParsePackage(std::move(uasset), std::move(uexp), &package, error)) {
        *error = "解析包失败: " + *error;
        return false;
    }

    log("包 " + package.package_name);
    log("  header=" + Num(package.header_size) + "  名称=" + Num(static_cast<int64_t>(package.names.size())) +
        "  导入=" + Num(static_cast<int64_t>(package.imports.size())) +
        "  导出=" + Num(static_cast<int64_t>(package.exports.size())) +
        "  uexp=" + Num(static_cast<int64_t>(package.uexp.size())) + " 字节");
    log("  条目 " + uasset_entry->name);

    for (const ExportObject& object : package.exports) {
        log("");
        log("[导出] " + object.name + "  (" + object.class_name + ")  序列 " +
            Num(object.serial_size) + " 字节 @ " + Num(object.serial_offset));
        std::vector<uint8_t> bytes;
        std::string export_error;
        if (!ExportBytes(package, object, &bytes, &export_error)) {
            log("    <定位失败: " + export_error + ">");
            continue;
        }
        const std::vector<PropertyTag> props = ParseProperties(bytes, package.names, 0);
        if (props.empty()) {
            log("    <没有解析到属性>");
            continue;
        }
        for (const PropertyTag& prop : props) {
            std::string line = "    " + prop.name + "  " + prop.type +
                               "  size=" + Num(prop.size);
            if (!prop.struct_name.empty()) {
                line += "  struct=" + prop.struct_name;
            }
            if (!prop.inner_type.empty()) {
                line += "  inner=" + prop.inner_type;
            }
            log(line);
        }
    }
    return true;
}

}  // namespace liveview
