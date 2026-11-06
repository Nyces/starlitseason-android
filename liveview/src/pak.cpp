#include "pak.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "aes256.h"

namespace liveview {
namespace {

constexpr uint32_t kPakMagic = 0x5A6F12E1;
constexpr int64_t kInfoTailScan = 2048;

#if defined(_WIN32)
// 用 Win32 文件 API 而不是 ifstream：std::ifstream 在 libc++/mingw 下没有宽路径重载。
struct FileReader {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int64_t size = 0;

    bool Open(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length)) {
            return false;
        }
        size = length.QuadPart;
        return true;
    }

    bool Read(int64_t offset, int64_t count, std::vector<uint8_t>* out) {
        if (count <= 0) {
            out->clear();
            return true;
        }
        LARGE_INTEGER position{};
        position.QuadPart = offset;
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
            return false;
        }
        out->resize(static_cast<size_t>(count));
        DWORD read = 0;
        if (!ReadFile(handle, out->data(), static_cast<DWORD>(count), &read, nullptr)) {
            return false;
        }
        return static_cast<int64_t>(read) == count;
    }

    void Close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }

    ~FileReader() { Close(); }
};
#endif

uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, 4);
    return value;
}

int32_t ReadI32(const uint8_t* data) {
    int32_t value = 0;
    std::memcpy(&value, data, 4);
    return value;
}

int64_t ReadI64(const uint8_t* data) {
    int64_t value = 0;
    std::memcpy(&value, data, 8);
    return value;
}

// FString：正长度是单字节（latin1），负长度是 UTF-16LE（长度取反，含结尾 0）。
bool ReadFString(const std::vector<uint8_t>& buffer, size_t* pos, std::string* out) {
    if (*pos + 4 > buffer.size()) {
        return false;
    }
    const int32_t length = ReadI32(buffer.data() + *pos);
    *pos += 4;
    if (length == 0) {
        out->clear();
        return true;
    }
    if (length < 0) {
        const size_t count = static_cast<size_t>(-length);
        if (*pos + count * 2 > buffer.size()) {
            return false;
        }
        // 只保留 ASCII 部分：条目名实际都是 ASCII，避免引入 UTF-16 转换
        out->clear();
        for (size_t i = 0; i < count; ++i) {
            const uint16_t ch = static_cast<uint16_t>(buffer[*pos + i * 2] |
                                                      (buffer[*pos + i * 2 + 1] << 8));
            if (ch == 0) {
                break;
            }
            out->push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        *pos += count * 2;
        return true;
    }
    const size_t count = static_cast<size_t>(length);
    if (*pos + count > buffer.size()) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(buffer.data() + *pos));
    while (!out->empty() && out->back() == '\0') {
        out->pop_back();
    }
    *pos += count;
    return true;
}

}  // namespace

bool ParseHexKey(const std::string& text, std::vector<uint8_t>* key) {
    std::string hex;
    hex.reserve(text.size());
    for (char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '-') {
            continue;
        }
        hex.push_back(ch);
    }
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }
    if (hex.size() != 64) {
        return false;
    }
    key->clear();
    for (size_t i = 0; i < 64; i += 2) {
        auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        key->push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

bool LoadPakIndex(const std::wstring& path, const std::vector<uint8_t>& key,
                  PakInfo* out, std::string* error) {
    FileReader file;
    if (!file.Open(path)) {
        *error = "无法打开 pak 文件";
        return false;
    }
    const int64_t file_size = file.size;
    if (file_size <= 0) {
        *error = "pak 文件为空";
        return false;
    }

    const int64_t scan = file_size < kInfoTailScan ? file_size : kInfoTailScan;
    std::vector<uint8_t> tail;
    if (!file.Read(file_size - scan, scan, &tail)) {
        *error = "读取 pak 尾部失败";
        return false;
    }

    size_t magic_pos = tail.size();
    for (size_t i = tail.size() >= 4 ? tail.size() - 4 : 0;; --i) {
        if (ReadU32(tail.data() + i) == kPakMagic) {
            magic_pos = i;
            break;
        }
        if (i == 0) {
            break;
        }
    }
    if (magic_pos >= tail.size()) {
        *error = "未找到 pak 魔数";
        return false;
    }
    const uint8_t* info = tail.data() + magic_pos;
    const size_t info_size = tail.size() - magic_pos;

    const int32_t version = ReadI32(info + 4);
    const int64_t index_offset = ReadI64(info + 8);
    const int64_t index_size = ReadI64(info + 16);
    const bool encrypted_index = info[44] != 0;

    if (index_size <= 0 || index_offset < 0 || index_offset + index_size > file_size) {
        *error = "索引范围越界";
        return false;
    }

    std::vector<uint8_t> index;
    if (!file.Read(index_offset, index_size, &index)) {
        *error = "读取索引失败";
        return false;
    }
    if (encrypted_index) {
        if (key.size() != 32) {
            *error = "索引已加密，但密钥缺失或长度不对";
            return false;
        }
        Aes256DecryptEcb(key.data(), index.data(), index.size());
    }

    size_t pos = 0;
    std::string mount_point;
    if (!ReadFString(index, &pos, &mount_point) || pos + 4 > index.size()) {
        *error = "索引头部损坏（密钥可能不对）";
        return false;
    }
    const int32_t count = ReadI32(index.data() + pos);
    pos += 4;
    if (count < 0 || count > 5000000) {
        *error = "条目数量异常（密钥可能不对）";
        return false;
    }

    out->path.clear();
    out->version = version;
    out->index_offset = index_offset;
    out->index_size = index_size;
    out->encrypted_index = encrypted_index;
    out->mount_point = mount_point;
    out->entries.clear();
    out->entries.reserve(static_cast<size_t>(count));

    for (int32_t i = 0; i < count; ++i) {
        PakEntry entry;
        if (!ReadFString(index, &pos, &entry.name) || pos + 24 > index.size()) {
            *error = "条目名解析失败（密钥可能不对）";
            return false;
        }
        entry.offset = ReadI64(index.data() + pos);
        entry.size = ReadI64(index.data() + pos + 8);
        entry.usize = ReadI64(index.data() + pos + 16);
        pos += 24;
        if (pos + 4 + 20 > index.size()) {
            *error = "条目头部越界";
            return false;
        }
        entry.method = ReadI32(index.data() + pos);
        pos += 4;
        pos += 20;  // SHA1
        if (entry.method != 0) {
            if (pos + 4 > index.size()) {
                *error = "压缩块表越界";
                return false;
            }
            const int32_t block_count = ReadI32(index.data() + pos);
            pos += 4;
            if (block_count < 0 || pos + static_cast<size_t>(block_count) * 16 > index.size()) {
                *error = "压缩块表损坏";
                return false;
            }
            entry.blocks.reserve(static_cast<size_t>(block_count));
            for (int32_t b = 0; b < block_count; ++b) {
                entry.blocks.emplace_back(ReadI64(index.data() + pos),
                                          ReadI64(index.data() + pos + 8));
                pos += 16;
            }
        }
        if (pos + 5 > index.size()) {
            *error = "条目尾部越界";
            return false;
        }
        entry.flags = index[pos];
        pos += 1;
        entry.block_size = ReadI32(index.data() + pos);
        pos += 4;
        out->entries.push_back(std::move(entry));
    }
    out->index_extra = static_cast<int64_t>(index.size()) - static_cast<int64_t>(pos);
    (void)info_size;
    return true;
}

}  // namespace liveview
