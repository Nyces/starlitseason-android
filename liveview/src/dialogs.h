// 原生文件/目录选择对话框。

#pragma once

#include <string>

namespace liveview {

// 文件夹选择框；返回 false 表示用户取消。
bool PickGameFolder(std::wstring* folder);

// 打开文件；filter 是 Win32 过滤器串（例如 L"相机包 (*.bundle)\0*.bundle\0所有文件\0*.*\0"）。
bool PickOpenFile(std::wstring* path, const wchar_t* filter);

// 保存文件；default_extension 不带点。
bool PickSaveFile(std::wstring* path, const wchar_t* filter, const wchar_t* default_extension);

}  // namespace liveview
