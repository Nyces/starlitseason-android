#include "dialogs.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shlobj.h>

namespace liveview {
namespace {

constexpr size_t kPathCapacity = MAX_PATH * 4;

void CopyToBuffer(const std::wstring& text, wchar_t* buffer) {
    const size_t count = text.size() < kPathCapacity - 1 ? text.size() : kPathCapacity - 1;
    if (count > 0) {
        std::memcpy(buffer, text.c_str(), count * sizeof(wchar_t));
    }
    buffer[count] = L'\0';
}

}  // namespace

bool PickGameFolder(std::wstring* folder) {
    wchar_t selected[MAX_PATH] = {0};
    BROWSEINFOW browse{};
    browse.hwndOwner = GetActiveWindow();
    browse.pszDisplayName = selected;
    browse.lpszTitle = L"选择游戏资源目录（包含 pak 文件的目录，例如 ...\\StarlitSeason\\Content\\Paks）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST item = SHBrowseForFolderW(&browse);
    if (item == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH] = {0};
    const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    if (!ok) {
        return false;
    }
    folder->assign(path);
    return true;
}

bool PickOpenFile(std::wstring* path, const wchar_t* filter) {
    wchar_t buffer[kPathCapacity] = {0};
    if (!path->empty()) {
        CopyToBuffer(*path, buffer);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(kPathCapacity);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return false;
    }
    path->assign(buffer);
    return true;
}

bool PickSaveFile(std::wstring* path, const wchar_t* filter, const wchar_t* default_extension) {
    wchar_t buffer[kPathCapacity] = {0};
    if (!path->empty()) {
        CopyToBuffer(*path, buffer);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(kPathCapacity);
    dialog.lpstrDefExt = default_extension;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&dialog) == FALSE) {
        return false;
    }
    path->assign(buffer);
    return true;
}

}  // namespace liveview
