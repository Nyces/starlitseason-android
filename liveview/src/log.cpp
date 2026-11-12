// 日志实现：先用 Win32 直接写文件（不做缓冲），崩溃时最后一行不会丢。

#include "log.h"

#include <cstring>
#include <cwchar>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include "config.h"

namespace liveview {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::string g_checkpoint;

void WriteRaw(const std::string& text) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(g_file);
}

std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute,
                  now.wSecond, now.wMilliseconds);
    return buffer;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string out(static_cast<size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            out.data(), length, nullptr, nullptr);
    }
    return out;
}

// 由异常地址找到所属模块，输出「模块名+偏移」，这样没有符号表也能定位。
std::string DescribeAddress(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.AllocationBase == nullptr) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%p", address);
        return buffer;
    }
    wchar_t path[MAX_PATH * 2] = {0};
    GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH * 2);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    const unsigned long long offset =
        reinterpret_cast<unsigned long long>(address) -
        reinterpret_cast<unsigned long long>(info.AllocationBase);
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llX", Narrow(name).c_str(), offset);
    return buffer;
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info != nullptr ? info->ExceptionRecord : nullptr;
    if (record == nullptr) {
        WriteRaw(Timestamp() + "[崩溃] 未知异常（没有异常记录）\r\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char buffer[768];
    std::snprintf(buffer, sizeof(buffer),
                  "[崩溃] 代码 0x%08lX 地址 %s 标志 0x%08lX\r\n",
                  static_cast<unsigned long>(record->ExceptionCode),
                  DescribeAddress(record->ExceptionAddress).c_str(),
                  static_cast<unsigned long>(record->ExceptionFlags));
    WriteRaw(Timestamp() + buffer);
    if (!g_checkpoint.empty()) {
        WriteRaw(Timestamp() + "[崩溃] 最近一步: " + g_checkpoint + "\r\n");
    }
    // 访问违例时把读写目标地址也记下来，区分空指针与野指针
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        std::snprintf(buffer, sizeof(buffer), "[崩溃] 访问类型 %lu 地址 0x%llX\r\n",
                      static_cast<unsigned long>(record->ExceptionInformation[0]),
                      static_cast<unsigned long long>(record->ExceptionInformation[1]));
        WriteRaw(Timestamp() + buffer);
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }

    // 顺手写一份 minidump，便于事后用调试器看清调用栈
    const std::wstring dump_path = LogPath() + L".dmp";
    HANDLE dump = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info_for_dump{};
        info_for_dump.ThreadId = GetCurrentThreadId();
        info_for_dump.ExceptionPointers = info;
        info_for_dump.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          static_cast<MINIDUMP_TYPE>(MiniDumpNormal), &info_for_dump,
                          nullptr, nullptr);
        CloseHandle(dump);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

std::wstring LogPath() {
    const std::wstring config = ConfigPath();
    const size_t slash = config.find_last_of(L"\\/");
    const std::wstring dir =
        slash == std::wstring::npos ? std::wstring() : config.substr(0, slash + 1);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"liveview.log";
}

std::string NarrowUtf8(const std::wstring& text) {
    return Narrow(text);
}

void InitLog() {
    if (g_file != INVALID_HANDLE_VALUE) {
        return;
    }
    const std::wstring path = LogPath();
    // 超过 4MB 就滚一份 old，避免长期使用后日志无限增长
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != 0 &&
        attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow > 4u * 1024u * 1024u) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(g_file, 0, nullptr, FILE_END);
    WriteRaw("\r\n===== liveview 启动 =====\r\n");
    WriteRaw(Timestamp() + "日志文件: " + Narrow(path) + "\r\n");
}

void LogText(const std::string& text) {
    if (g_file == INVALID_HANDLE_VALUE) {
        InitLog();
    }
    WriteRaw(Timestamp() + text + "\r\n");
}

void SetCheckpoint(const std::string& text) {
    g_checkpoint = text;
}

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashHandler);
}

}  // namespace liveview
