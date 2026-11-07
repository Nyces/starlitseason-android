// UE4 pak v8 索引读取（里程碑 1 只需要解析索引，不读条目数据）。
//
// 格式实测确认：
//   FPakInfo 在文件尾部，魔数 0x5A6F12E1；
//   索引可被 AES-256-ECB 加密（索引不压缩）；
//   条目名是 FString，条目数据按 64KB 分块压缩（后续里程碑再解压）。

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace liveview {

struct PakEntry {
    std::string name;
    int64_t offset = 0;
    int64_t size = 0;
    int64_t usize = 0;
    int32_t method = 0;
    uint8_t flags = 0;
    int32_t block_size = 0;
    std::vector<std::pair<int64_t, int64_t>> blocks;
};

struct PakInfo {
    std::wstring wide_path;  // 读条目时用的原始宽路径
    std::string path;        // 供日志显示的窄路径
    int32_t version = 0;
    int64_t index_offset = 0;
    int64_t index_size = 0;
    bool encrypted_index = false;
    std::string mount_point;
    int64_t index_extra = 0;
    std::vector<PakEntry> entries;
};

// key 为空表示索引未加密（本作实际都是加密的）。失败时填充 error。
bool LoadPakIndex(const std::wstring& path, const std::vector<uint8_t>& key,
                  PakInfo* out, std::string* error);

// 把 64 位十六进制串（可带 0x）解析成 32 字节密钥。
bool ParseHexKey(const std::string& text, std::vector<uint8_t>* key);

// 按名字精确查找条目。
const PakEntry* FindEntry(const PakInfo& pak, const std::string& name);

// 读取并解压条目数据。
// 未压缩条目：数据从 offset+53 开始；压缩条目：块偏移相对条目起点，
// 加密时块长按 16 对齐后整体 AES-256-ECB 解密，再逐块 zlib 解压。
bool ReadEntry(const PakInfo& pak, const PakEntry& entry, const std::vector<uint8_t>& key,
               std::vector<uint8_t>* out, std::string* error);

}  // namespace liveview
