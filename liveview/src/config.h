// 本机配置：游戏资源目录 + AES 密钥 + 上次用的相机包（%LOCALAPPDATA%\liveview\config.ini）。

#pragma once

#include <string>

namespace liveview {

struct Config {
    std::wstring game_dir;
    std::string key_hex;
    std::wstring bundle_path;
    std::wstring show_path;   // 演出包（*.lvs）
};

std::wstring ConfigPath();
bool LoadConfig(Config* config);
bool SaveConfig(const Config& config);

}  // namespace liveview
