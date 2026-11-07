// UE4.24 cooked 包解析：包头、名称表、导入/导出表、FPropertyTag 属性流。
//
// 与 Python 侧 tools/sspak/ue4/package.py 对齐：
//   零售包是 unversioned（包头版本字段为 0），按 4.24 布局解析；
//   导出表的 SerialOffset 相对「uasset 包头 + uexp」拼接起点，
//   所以 uexp 内偏移 = SerialOffset - TotalHeaderSize。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace liveview {

struct ImportObject {
    std::string class_package;
    std::string class_name;
    int32_t outer_index = 0;
    std::string object_name;
};

struct ExportObject {
    std::string name;
    int32_t class_index = 0;
    int32_t outer_index = 0;
    int64_t serial_size = 0;
    int64_t serial_offset = 0;
    std::string class_name;
};

struct PropertyTag {
    std::string name;
    std::string type;
    int32_t size = 0;
    int32_t array_index = 0;
    size_t offset = 0;  // 值起点
    std::string struct_name;
    std::string inner_type;
};

struct PackageData {
    std::vector<uint8_t> uasset;  // .uasset（包头部分）
    std::vector<uint8_t> uexp;    // .uexp（导出数据）
    std::string package_name;
    int32_t header_size = 0;
    std::vector<std::string> names;
    std::vector<ImportObject> imports;
    std::vector<ExportObject> exports;
};

// 解析包；失败返回 false 并填 error。
bool ParsePackage(std::vector<uint8_t> uasset, std::vector<uint8_t> uexp, PackageData* out,
                  std::string* error);

// 导出对象的数据切片（定位到 uexp）。
bool ExportBytes(const PackageData& package, const ExportObject& object,
                 std::vector<uint8_t>* out, std::string* error);

// 解析一条属性标签；is_none 为 true 表示读到 None 终止符。
bool ParsePropertyTag(const std::vector<uint8_t>& data, const std::vector<std::string>& names,
                      size_t pos, PropertyTag* out, size_t* next, bool* is_none,
                      std::string* error);

// 从 start 起解析一串属性，直到 None。
std::vector<PropertyTag> ParseProperties(const std::vector<uint8_t>& data,
                                        const std::vector<std::string>& names, size_t start);

std::string NameAt(const std::vector<std::string>& names, int32_t index);

const ExportObject* FindExport(const PackageData& package, const std::string& class_name);

}  // namespace liveview
