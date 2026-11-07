#include "bundle.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace liveview {
namespace {

bool SplitKeyValue(const std::string& line, std::string* key, std::string* rest) {
    std::istringstream stream(line);
    if (!(stream >> *key)) {
        return false;
    }
    std::getline(stream, *rest);
    if (!rest->empty() && rest->front() == ' ') {
        rest->erase(rest->begin());
    }
    return true;
}

bool ParseBundleText(const std::string& text, Bundle* bundle, std::string* error) {
    std::istringstream input(text);
    bundle->keys.clear();
    std::string line;
    int version = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            std::string name;
            std::string rest;
            if (!SplitKeyValue(line.substr(1), &name, &rest)) {
                continue;
            }
            std::istringstream stream(rest);
            if (name == "liveview-bundle") {
                stream >> version;
            } else if (name == "asset") {
                bundle->asset = rest;
            } else if (name == "binding") {
                bundle->binding = rest;
            } else if (name == "tick") {
                stream >> bundle->tick_fps;
            } else if (name == "range") {
                stream >> bundle->range_start >> bundle->range_end >> bundle->range_step;
            }
            continue;
        }
        std::istringstream stream(line);
        std::string tag;
        CameraKey key;
        if (!(stream >> tag >> key.frame)) {
            *error = "无法解析数据行: " + line;
            return false;
        }
        if (tag != "F") {
            *error = "未知的记录类型: " + tag;
            return false;
        }
        for (double& value : key.position) {
            stream >> value;
        }
        for (double& value : key.target) {
            stream >> value;
        }
        bundle->keys.push_back(key);
    }
    if (version != 1) {
        *error = "文本包版本不支持（需要 1）";
        return false;
    }
    if (bundle->keys.empty()) {
        *error = "文本包没有任何机位关键帧";
        return false;
    }
    std::sort(bundle->keys.begin(), bundle->keys.end(),
              [](const CameraKey& a, const CameraKey& b) { return a.frame < b.frame; });
    return true;
}

}  // namespace

bool LoadBundle(const std::string& path, Bundle* bundle, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "无法打开文本包: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return ParseBundleText(buffer.str(), bundle, error);
}

bool LoadBundleW(const std::wstring& path, Bundle* bundle, std::string* error) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = "无法打开文本包";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0) {
        CloseHandle(handle);
        *error = "文本包为空或读取失败";
        return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read != static_cast<DWORD>(text.size())) {
        *error = "读取文本包失败";
        return false;
    }
    return ParseBundleText(text, bundle, error);
}

CameraKey SampleCamera(const Bundle& bundle, double frame) {
    if (bundle.keys.empty()) {
        return CameraKey{};
    }
    if (frame <= static_cast<double>(bundle.keys.front().frame)) {
        return bundle.keys.front();
    }
    if (frame >= static_cast<double>(bundle.keys.back().frame)) {
        return bundle.keys.back();
    }
    for (size_t i = 1; i < bundle.keys.size(); ++i) {
        const CameraKey& right = bundle.keys[i];
        if (frame <= static_cast<double>(right.frame)) {
            const CameraKey& left = bundle.keys[i - 1];
            const double span = static_cast<double>(right.frame - left.frame);
            const double t = span > 0.0 ? (frame - static_cast<double>(left.frame)) / span : 0.0;
            CameraKey out;
            out.frame = static_cast<int64_t>(frame);
            for (int axis = 0; axis < 3; ++axis) {
                out.position[axis] =
                    left.position[axis] + (right.position[axis] - left.position[axis]) * t;
                out.target[axis] =
                    left.target[axis] + (right.target[axis] - left.target[axis]) * t;
            }
            return out;
        }
    }
    return bundle.keys.back();
}

}  // namespace liveview
