// 启动时的两个原生对话框：选游戏资源目录、输入 AES 密钥。

#pragma once

#include <string>

namespace liveview {

// 调 Windows 文件夹选择框；返回 false 表示用户取消。
bool PickGameFolder(std::wstring* folder);

// 弹出一个输入框要求输入 64 位十六进制密钥；返回 false 表示取消。
bool AskAesKey(std::string* key_hex, const std::string& initial);

}  // namespace liveview
