// AES-256 ECB 解密（只实现解密，够 pak 索引用）。
//
// 不引入 OpenSSL/BCrypt：自有实现约 120 行，构建零依赖，也便于用 Zig 交叉检查。
// 表在首次使用时按 GF(2^8) 生成，避免手抄 512 个常量出错。

#pragma once

#include <cstddef>
#include <cstdint>

namespace liveview {

// key 必须是 32 字节。round_keys 输出 240 字节（15 轮 × 16）。
void Aes256ExpandKey(const uint8_t key[32], uint8_t round_keys[240]);

// 解密一个 16 字节分组。
void Aes256DecryptBlock(const uint8_t round_keys[240], const uint8_t in[16], uint8_t out[16]);

// 原地解密整段数据（只处理 16 的整数倍，尾部不足 16 字节的部分保持原样）。
void Aes256DecryptEcb(const uint8_t key[32], uint8_t* data, size_t size);

}  // namespace liveview
