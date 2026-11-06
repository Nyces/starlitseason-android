#include "dialogs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "pak.h"

namespace liveview {
namespace {

struct PromptState {
    HWND window = nullptr;
    HWND edit = nullptr;
    std::string* out = nullptr;
    bool done = false;
    bool accepted = false;
};

PromptState* g_prompt = nullptr;

void AcceptPrompt() {
    wchar_t text[256] = {0};
    GetWindowTextW(g_prompt->edit, text, 256);
    std::string narrow;
    for (const wchar_t* p = text; *p != 0; ++p) {
        narrow.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
    }
    std::vector<uint8_t> key;
    if (!ParseHexKey(narrow, &key)) {
        MessageBoxW(g_prompt->window,
                    L"AES 密钥必须是 64 位十六进制（32 字节）。\n例如 0x 开头的 64 个字符。",
                    L"liveview", MB_OK | MB_ICONWARNING);
        return;
    }
    *g_prompt->out = narrow;
    g_prompt->accepted = true;
    g_prompt->done = true;
}

LRESULT CALLBACK PromptProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                AcceptPrompt();
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                g_prompt->done = true;
                return 0;
            }
            return 0;
        case WM_CLOSE:
            g_prompt->done = true;
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
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

bool AskAesKey(std::string* key_hex, const std::string& initial) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.lpfnWndProc = PromptProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"liveview_key_prompt";
    RegisterClassExW(&window_class);

    PromptState state;
    state.out = key_hex;
    g_prompt = &state;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rect = {0, 0, 520, 150};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    state.window = CreateWindowExW(0, L"liveview_key_prompt", L"输入 AES 密钥", style, x, y,
                                   width, height, nullptr, nullptr, instance, nullptr);
    if (state.window == nullptr) {
        g_prompt = nullptr;
        return false;
    }

    CreateWindowExW(0, L"STATIC", L"64 位十六进制密钥（会保存到本机 %LOCALAPPDATA%\\liveview\\config.ini）：",
                    WS_CHILD | WS_VISIBLE, 16, 14, 480, 20, state.window, nullptr, instance, nullptr);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 16, 40, 488, 24, state.window, nullptr, instance, nullptr);
    std::wstring initial_wide(initial.begin(), initial.end());
    SetWindowTextW(state.edit, initial_wide.c_str());
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    376, 90, 128, 34, state.window, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    248, 90, 120, 34, state.window, reinterpret_cast<HMENU>(IDCANCEL), instance,
                    nullptr);

    ShowWindow(state.window, SW_SHOW);
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    const bool accepted = state.accepted;
    if (IsWindow(state.window)) {
        DestroyWindow(state.window);
    }
    g_prompt = nullptr;
    return accepted;
}

}  // namespace liveview
