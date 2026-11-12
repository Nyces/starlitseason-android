// 统一日志：所有关键步骤同时写界面与磁盘文件，崩溃时也能留痕。
//
// 文件位置：%LOCALAPPDATA%\liveview\liveview.log
// 每次启动追加一段，带时间戳；崩溃时由异常过滤器补一行 [崩溃]。

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace liveview {

// 打开日志文件（追加）。启动时调用一次，可重复调用。
void InitLog();

// 写一行日志（自动加时间戳与换行，并立即落盘）。
void LogText(const std::string& text);

// 记录「最近一步」，崩溃时随异常信息一起写出，便于定位。
void SetCheckpoint(const std::string& text);

// 安装未处理异常过滤器：崩溃时写日志并正常退出，避免无提示闪退。
void InstallCrashHandler();

// 日志文件路径。
std::wstring LogPath();

// 宽串 -> UTF-8 窄串（日志统一用 UTF-8）。
std::string NarrowUtf8(const std::wstring& text);

inline void Logf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogText(buffer);
}

}  // namespace liveview
