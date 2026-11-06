#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace liveview {
namespace {

constexpr wchar_t kSection[] = L"liveview";
constexpr wchar_t kKeyDir[] = L"game_dir";
constexpr wchar_t kKeyAes[] = L"aes_key";

std::wstring BaseDirectory() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH * 2);
    if (length == 0 || length >= MAX_PATH * 2) {
        length = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH * 2);
    }
    if (length == 0 || length >= MAX_PATH * 2) {
        return L".";
    }
    std::wstring base(buffer, length);
    base += L"\\liveview";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

}  // namespace

std::wstring ConfigPath() {
    return BaseDirectory() + L"\\config.ini";
}

bool LoadConfig(Config* config) {
    const std::wstring path = ConfigPath();
    wchar_t dir[MAX_PATH * 2] = {0};
    if (GetPrivateProfileStringW(kSection, kKeyDir, L"", dir, MAX_PATH * 2, path.c_str()) == 0) {
        return false;
    }
    wchar_t aes[256] = {0};
    if (GetPrivateProfileStringW(kSection, kKeyAes, L"", aes, 256, path.c_str()) == 0) {
        return false;
    }
    config->game_dir.assign(dir);
    config->key_hex.clear();
    for (const wchar_t* p = aes; *p != 0; ++p) {
        config->key_hex.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    }
    return true;
}

bool SaveConfig(const Config& config) {
    const std::wstring path = ConfigPath();
    std::wstring key(config.key_hex.begin(), config.key_hex.end());
    const BOOL dir_ok = WritePrivateProfileStringW(kSection, kKeyDir, config.game_dir.c_str(),
                                                   path.c_str());
    const BOOL key_ok = WritePrivateProfileStringW(kSection, kKeyAes, key.c_str(), path.c_str());
    return dir_ok != FALSE && key_ok != FALSE;
}

}  // namespace liveview
