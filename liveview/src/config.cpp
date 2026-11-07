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
constexpr wchar_t kKeyBundle[] = L"bundle_path";

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

std::wstring ReadValue(const std::wstring& path, const wchar_t* key) {
    wchar_t buffer[MAX_PATH * 2] = {0};
    if (GetPrivateProfileStringW(kSection, key, L"", buffer, MAX_PATH * 2, path.c_str()) == 0) {
        return std::wstring();
    }
    return std::wstring(buffer);
}

}  // namespace

std::wstring ConfigPath() {
    return BaseDirectory() + L"\\config.ini";
}

bool LoadConfig(Config* config) {
    const std::wstring path = ConfigPath();
    config->game_dir = ReadValue(path, kKeyDir);
    const std::wstring aes = ReadValue(path, kKeyAes);
    config->key_hex.clear();
    for (wchar_t ch : aes) {
        config->key_hex.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    config->bundle_path = ReadValue(path, kKeyBundle);
    return !config->game_dir.empty() || !config->key_hex.empty();
}

bool SaveConfig(const Config& config) {
    const std::wstring path = ConfigPath();
    const std::wstring key(config.key_hex.begin(), config.key_hex.end());
    const BOOL ok_dir =
        WritePrivateProfileStringW(kSection, kKeyDir, config.game_dir.c_str(), path.c_str());
    const BOOL ok_key =
        WritePrivateProfileStringW(kSection, kKeyAes, key.c_str(), path.c_str());
    const BOOL ok_bundle = WritePrivateProfileStringW(kSection, kKeyBundle,
                                                      config.bundle_path.c_str(), path.c_str());
    return ok_dir != FALSE && ok_key != FALSE && ok_bundle != FALSE;
}

}  // namespace liveview
