// 验证工具的主窗口：所有参数都在界面里设置，不需要命令行。
//
// 界面分四块：
//   1) 游戏资源目录 + AES 密钥（记住到 %LOCALAPPDATA%\liveview\config.ini）
//   2) 条目操作：按关键字列出、按名字导出
//   3) 资产：解析 .uasset/.uexp，打印导出表与属性标签
//   4) 播放：载入相机包，用 Vulkan 后端按时间轴播放
// 底部是日志框。

#include "app_window.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "bundle.h"
#include "commands.h"
#include "config.h"
#include "dialogs.h"
#include "pak.h"
#include "show.h"

#ifdef LIVEVIEW_RENDER
#include "render_vulkan.h"
#endif

namespace liveview {
namespace {

enum ControlId {
    kIdDirLabel = 1001,
    kIdDirEdit,
    kIdDirBrowse,
    kIdKeyLabel,
    kIdKeyEdit,
    kIdCheck,
    kIdGroupEntries,
    kIdFilterLabel,
    kIdFilterEdit,
    kIdList,
    kIdEntryLabel,
    kIdEntryEdit,
    kIdExtract,
    kIdGroupAsset,
    kIdAssetLabel,
    kIdAssetEdit,
    kIdDump,
    kIdGroupPlay,
    kIdBundleLabel,
    kIdBundleEdit,
    kIdBundleBrowse,
    kIdPlay,
    kIdShowLabel,
    kIdShowEdit,
    kIdShowBrowse,
    kIdShowPlay,
    kIdClearLog,
    kIdLog,
};

struct AppState {
    HWND window = nullptr;

    HWND dir_label = nullptr;
    HWND dir_edit = nullptr;
    HWND dir_browse = nullptr;
    HWND key_label = nullptr;
    HWND key_edit = nullptr;
    HWND check = nullptr;

    HWND group_entries = nullptr;
    HWND filter_label = nullptr;
    HWND filter_edit = nullptr;
    HWND list = nullptr;
    HWND entry_label = nullptr;
    HWND entry_edit = nullptr;
    HWND extract = nullptr;

    HWND group_asset = nullptr;
    HWND asset_label = nullptr;
    HWND asset_edit = nullptr;
    HWND dump = nullptr;

    HWND group_play = nullptr;
    HWND bundle_label = nullptr;
    HWND bundle_edit = nullptr;
    HWND bundle_browse = nullptr;
    HWND play = nullptr;
    HWND show_label = nullptr;
    HWND show_edit = nullptr;
    HWND show_browse = nullptr;
    HWND show_play = nullptr;

    HWND clear_log = nullptr;
    HWND log = nullptr;

    HFONT font = nullptr;
    HFONT mono = nullptr;
    HBRUSH background = nullptr;

    // pak 索引缓存：目录和密钥都没变时复用，避免每次操作重读
    std::wstring paks_dir;
    std::string paks_key;
    std::vector<PakInfo> paks;
};

// 取控件文本；缓冲区留出结尾 0 的位置，否则 GetWindowTextW 会越界一个字符。
std::wstring GetText(HWND control) {
    if (control == nullptr) {
        return std::wstring();
    }
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void SetText(HWND control, const std::wstring& text) {
    if (control != nullptr) {
        SetWindowTextW(control, text.c_str());
    }
}

void AppendLog(AppState* state, const std::wstring& line) {
    if (state->log == nullptr) {
        return;
    }
    const int length = GetWindowTextLengthW(state->log);
    std::wstring text = line + L"\r\n";
    // 只读编辑框直接 EM_REPLACESEL 会被忽略，先临时解除只读
    SendMessageW(state->log, EM_SETREADONLY, FALSE, 0);
    SendMessageW(state->log, EM_SETSEL, static_cast<WPARAM>(length),
                 static_cast<LPARAM>(length));
    SendMessageW(state->log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(state->log, EM_SETREADONLY, TRUE, 0);
    SendMessageW(state->log, EM_SCROLLCARET, 0, 0);
}

LogSink MakeSink(AppState* state) {
    return [state](const std::string& line) {
        AppendLog(state, WidenUtf8(line));
        // 让日志在长操作中也能及时刷新
        UpdateWindow(state->log);
    };
}

void ClearLog(AppState* state) {
    if (state->log != nullptr) {
        SetWindowTextW(state->log, L"");
    }
}

AppState* StateOf(HWND window) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void SaveCurrentConfig(AppState* state) {
    Config config;
    config.game_dir = GetText(state->dir_edit);
    config.key_hex = NarrowAscii(GetText(state->key_edit));
    config.bundle_path = GetText(state->bundle_edit);
    config.show_path = GetText(state->show_edit);
    SaveConfig(config);
}

// 校验目录与密钥并加载索引；目录/密钥没变时复用缓存。
bool EnsurePaks(AppState* state, bool force_reload) {
    const std::wstring directory = GetText(state->dir_edit);
    const std::string key_hex = NarrowAscii(GetText(state->key_edit));
    if (directory.empty()) {
        AppendLog(state, L"[错误] 请先选择游戏资源目录");
        return false;
    }
    std::vector<uint8_t> key;
    if (!ParseHexKey(key_hex, &key)) {
        AppendLog(state, L"[错误] AES 密钥必须是 64 位十六进制（32 字节）");
        return false;
    }
    if (!force_reload && !state->paks.empty() && directory == state->paks_dir &&
        key_hex == state->paks_key) {
        return true;
    }

    state->paks.clear();
    state->paks_dir.clear();
    state->paks_key.clear();

    std::string error;
    if (!LoadAllPaks(directory, key, &state->paks, MakeSink(state), &error)) {
        AppendLog(state, L"[错误] " + WidenUtf8(error));
        return false;
    }
    state->paks_dir = directory;
    state->paks_key = key_hex;
    SaveCurrentConfig(state);
    return true;
}

HWND MakeControl(AppState* state, const wchar_t* window_class, const wchar_t* text, DWORD style,
                 int id) {
    HWND control = CreateWindowExW(
        0, window_class, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, state->window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && state->font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    }
    return control;
}

void Place(HWND control, int x, int y, int width, int height) {
    if (control != nullptr) {
        MoveWindow(control, x, y, width, height, TRUE);
    }
}

void LayoutControls(AppState* state, int width, int height) {
    const int margin = 12;
    const int row_height = 24;
    const int gap = 6;
    const int label_width = 92;
    const int button_width = 104;

    const int field_x = margin + label_width + gap;
    const int field_width = width - field_x - button_width - gap - margin;

    int y = margin;

    // 行 1：游戏资源目录
    Place(state->dir_label, margin, y + 4, label_width, 18);
    Place(state->dir_edit, field_x, y, field_width, row_height);
    Place(state->dir_browse, width - margin - button_width, y, button_width, row_height);
    y += row_height + gap;

    // 行 2：AES 密钥 + 校验
    Place(state->key_label, margin, y + 4, label_width, 18);
    Place(state->key_edit, field_x, y, field_width, row_height);
    Place(state->check, width - margin - button_width, y, button_width, row_height);
    y += row_height + gap + 4;

    // 分组：条目
    const int entries_height = 84;
    Place(state->group_entries, margin, y, width - margin * 2, entries_height);
    const int inner_x = margin + 80;
    const int inner_width = width - inner_x - button_width - gap - margin - 12;
    int inner_y = y + 22;
    Place(state->filter_label, margin + 12, inner_y + 4, 60, 18);
    Place(state->filter_edit, inner_x, inner_y, inner_width, row_height);
    Place(state->list, width - margin - 12 - button_width, inner_y, button_width, row_height);
    inner_y += row_height + gap;
    Place(state->entry_label, margin + 12, inner_y + 4, 60, 18);
    Place(state->entry_edit, inner_x, inner_y, inner_width, row_height);
    Place(state->extract, width - margin - 12 - button_width, inner_y, button_width, row_height);
    y += entries_height + gap;

    // 分组：资产
    const int asset_height = 56;
    Place(state->group_asset, margin, y, width - margin * 2, asset_height);
    inner_y = y + 22;
    Place(state->asset_label, margin + 12, inner_y + 4, 60, 18);
    Place(state->asset_edit, inner_x, inner_y, inner_width, row_height);
    Place(state->dump, width - margin - 12 - button_width, inner_y, button_width, row_height);
    y += asset_height + gap;

    // 分组：播放（相机包 + 完整演出）
    const int play_height = 84;
    Place(state->group_play, margin, y, width - margin * 2, play_height);
    inner_y = y + 22;
    const int small_button = 72;
    const int bundle_width =
        width - inner_x - small_button * 2 - gap * 2 - margin - 12;
    Place(state->bundle_label, margin + 12, inner_y + 4, 60, 18);
    Place(state->bundle_edit, inner_x, inner_y, bundle_width > 80 ? bundle_width : 80, row_height);
    const int browse_x = inner_x + (bundle_width > 80 ? bundle_width : 80) + gap;
    Place(state->bundle_browse, browse_x, inner_y, small_button, row_height);
    Place(state->play, browse_x + small_button + gap, inner_y, small_button, row_height);
    inner_y += row_height + gap;
    Place(state->show_label, margin + 12, inner_y + 4, 60, 18);
    Place(state->show_edit, inner_x, inner_y, bundle_width > 80 ? bundle_width : 80, row_height);
    Place(state->show_browse, browse_x, inner_y, small_button, row_height);
    Place(state->show_play, browse_x + small_button + gap, inner_y, small_button, row_height);
    y += play_height + gap;

    // 日志
    Place(state->clear_log, width - margin - 96, y, 96, 22);
    y += 26;
    int log_height = height - y - margin;
    if (log_height < 80) {
        log_height = 80;
    }
    Place(state->log, margin, y, width - margin * 2, log_height);
}

void OnCheckPaks(AppState* state) {
    AppendLog(state, L"—— 校验 pak 索引 ——");
    if (!EnsurePaks(state, true)) {
        return;
    }
    SummarizePaks(state->paks, MakeSink(state));
}

void OnListEntries(AppState* state) {
    if (!EnsurePaks(state, false)) {
        return;
    }
    const std::string filter = NarrowAscii(GetText(state->filter_edit));
    AppendLog(state, L"—— 列出条目（关键字：" + WidenUtf8(filter) + L"）——");
    ListEntries(state->paks, filter, 500, MakeSink(state));
}

void OnExtract(AppState* state) {
    if (!EnsurePaks(state, false)) {
        return;
    }
    const std::string name = NarrowAscii(GetText(state->entry_edit));
    if (name.empty()) {
        AppendLog(state, L"[错误] 请填写要导出的条目名（可以只写后缀，如 S_chr_body_base.uasset）");
        return;
    }
    std::wstring output = GetText(state->dir_edit);
    if (!output.empty()) {
        output += L"\\";
    }
    const size_t slash = name.find_last_of('/');
    const std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
    output += std::wstring(leaf.begin(), leaf.end());
    if (!PickSaveFile(&output, L"所有文件\0*.*\0", L"bin")) {
        return;
    }
    const std::string key_hex = NarrowAscii(GetText(state->key_edit));
    std::vector<uint8_t> key;
    if (!ParseHexKey(key_hex, &key)) {
        AppendLog(state, L"[错误] AES 密钥非法");
        return;
    }
    std::string error;
    if (!ExtractEntry(state->paks, key, name, output, MakeSink(state), &error)) {
        AppendLog(state, L"[错误] " + WidenUtf8(error));
    }
}

void OnDumpAsset(AppState* state) {
    if (!EnsurePaks(state, false)) {
        return;
    }
    const std::string asset = NarrowAscii(GetText(state->asset_edit));
    if (asset.empty()) {
        AppendLog(state, L"[错误] 请填写资产路径，例如 Model/Character/Body/Base/chr_body_base/Mesh/S_chr_body_base");
        return;
    }
    const std::string key_hex = NarrowAscii(GetText(state->key_edit));
    std::vector<uint8_t> key;
    if (!ParseHexKey(key_hex, &key)) {
        AppendLog(state, L"[错误] AES 密钥非法");
        return;
    }
    AppendLog(state, L"—— 解析资产 " + WidenUtf8(asset) + L" ——");
    std::string error;
    if (!DumpAsset(state->paks, key, asset, MakeSink(state), &error)) {
        AppendLog(state, L"[错误] " + WidenUtf8(error));
    }
}

void OnPlay(AppState* state) {
    const std::wstring path = GetText(state->bundle_edit);
    if (path.empty()) {
        AppendLog(state, L"[错误] 请先选择相机包（*.bundle）");
        return;
    }
    Bundle bundle;
    std::string error;
    if (!LoadBundleW(path, &bundle, &error)) {
        AppendLog(state, L"[错误] 读取相机包失败：" + WidenUtf8(error));
        return;
    }
    AppendLog(state, L"相机包：" + path);
    AppendLog(state, L"  资源 " + WidenUtf8(bundle.asset) + L"，绑定 " +
                         WidenUtf8(bundle.binding));
    AppendLog(state, L"  范围 " + std::to_wstring(bundle.range_start) + L" .. " +
                         std::to_wstring(bundle.range_end) + L"，关键帧 " +
                         std::to_wstring(bundle.keys.size()) + L" 个");
    SaveCurrentConfig(state);
#ifdef LIVEVIEW_RENDER
    AppendLog(state, L"打开播放窗口（ESC 退出）…");
    const int code = RunRender(bundle, -1.0);
    AppendLog(state, L"播放结束，退出码 " + std::to_wstring(code));
#else
    AppendLog(state, L"本构建未启用渲染后端：需用 -DLIVEVIEW_RENDER=ON 重新构建");
#endif
}

void OnPlayShow(AppState* state) {
    const std::wstring path = GetText(state->show_edit);
    if (path.empty()) {
        AppendLog(state, L"[错误] 请先选择演出包（*.lvs，由 sspak live-show 生成）");
        return;
    }
    Show show;
    std::string error;
    if (!show.Load(path, &error)) {
        AppendLog(state, L"[错误] 读取演出包失败：" + WidenUtf8(error));
        return;
    }
    AppendLog(state, L"演出包：" + path);
    AppendLog(state, L"  时长 " + std::to_wstring(static_cast<int>(show.duration + 0.5)) +
                         L" 秒，舞台 " + std::to_wstring(show.StageTriangleCount()) +
                         L" 三角，角色 " + std::to_wstring(show.SkinTriangleCount()) +
                         L" 三角，动画 " + std::to_wstring(show.anim_bones.size()) + L" 骨骼");
    SaveCurrentConfig(state);
#ifdef LIVEVIEW_RENDER
    AppendLog(state, L"打开演出窗口（ESC 退出）…");
    const int code = RunShow(show);
    AppendLog(state, L"演出窗口已关闭，退出码 " + std::to_wstring(code));
#else
    AppendLog(state, L"本构建未启用渲染后端：需用 -DLIVEVIEW_RENDER=ON 重新构建");
#endif
}

void OnCommand(AppState* state, int id) {
    switch (id) {
        case kIdDirBrowse: {
            std::wstring folder = GetText(state->dir_edit);
            if (PickGameFolder(&folder)) {
                SetText(state->dir_edit, folder);
                state->paks.clear();
                state->paks_dir.clear();
                state->paks_key.clear();
                SaveCurrentConfig(state);
                AppendLog(state, L"游戏目录：" + folder);
            }
            return;
        }
        case kIdCheck:
            OnCheckPaks(state);
            return;
        case kIdList:
            OnListEntries(state);
            return;
        case kIdExtract:
            OnExtract(state);
            return;
        case kIdDump:
            OnDumpAsset(state);
            return;
        case kIdBundleBrowse: {
            std::wstring path = GetText(state->bundle_edit);
            if (PickOpenFile(&path, L"相机包 (*.bundle)\0*.bundle\0所有文件\0*.*\0")) {
                SetText(state->bundle_edit, path);
                SaveCurrentConfig(state);
            }
            return;
        }
        case kIdPlay:
            OnPlay(state);
            return;
        case kIdShowBrowse: {
            std::wstring path = GetText(state->show_edit);
            if (PickOpenFile(&path, L"演出包 (*.lvs)\0*.lvs\0所有文件\0*.*\0")) {
                SetText(state->show_edit, path);
                SaveCurrentConfig(state);
            }
            return;
        }
        case kIdShowPlay:
            OnPlayShow(state);
            return;
        case kIdClearLog:
            ClearLog(state);
            return;
        default:
            return;
    }
}

void CreateControls(AppState* state) {
    state->dir_label = MakeControl(state, L"STATIC", L"游戏资源目录", SS_LEFT, kIdDirLabel);
    state->dir_edit = MakeControl(state, L"EDIT", L"",
                                  WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdDirEdit);
    state->dir_browse = MakeControl(state, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
                                    kIdDirBrowse);

    state->key_label = MakeControl(state, L"STATIC", L"AES 密钥", SS_LEFT, kIdKeyLabel);
    state->key_edit = MakeControl(state, L"EDIT", L"",
                                  WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdKeyEdit);
    state->check = MakeControl(state, L"BUTTON", L"校验 pak 索引",
                               BS_PUSHBUTTON | WS_TABSTOP, kIdCheck);

    state->group_entries = MakeControl(state, L"BUTTON", L"条目", BS_GROUPBOX, kIdGroupEntries);
    state->filter_label = MakeControl(state, L"STATIC", L"关键字", SS_LEFT, kIdFilterLabel);
    state->filter_edit = MakeControl(state, L"EDIT", L"",
                                     WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdFilterEdit);
    state->list = MakeControl(state, L"BUTTON", L"列出条目", BS_PUSHBUTTON | WS_TABSTOP, kIdList);
    state->entry_label = MakeControl(state, L"STATIC", L"条目名", SS_LEFT, kIdEntryLabel);
    state->entry_edit = MakeControl(state, L"EDIT", L"",
                                    WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdEntryEdit);
    state->extract = MakeControl(state, L"BUTTON", L"导出到…", BS_PUSHBUTTON | WS_TABSTOP,
                                 kIdExtract);

    state->group_asset = MakeControl(state, L"BUTTON", L"资产", BS_GROUPBOX, kIdGroupAsset);
    state->asset_label = MakeControl(state, L"STATIC", L"资产路径", SS_LEFT, kIdAssetLabel);
    state->asset_edit = MakeControl(state, L"EDIT", L"",
                                    WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdAssetEdit);
    state->dump = MakeControl(state, L"BUTTON", L"解析结构", BS_PUSHBUTTON | WS_TABSTOP, kIdDump);

    state->group_play = MakeControl(state, L"BUTTON", L"播放", BS_GROUPBOX, kIdGroupPlay);
    state->bundle_label = MakeControl(state, L"STATIC", L"相机包", SS_LEFT, kIdBundleLabel);
    state->bundle_edit = MakeControl(state, L"EDIT", L"",
                                     WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdBundleEdit);
    state->bundle_browse = MakeControl(state, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
                                       kIdBundleBrowse);
    state->play = MakeControl(state, L"BUTTON", L"播放", BS_PUSHBUTTON | WS_TABSTOP, kIdPlay);
    state->show_label = MakeControl(state, L"STATIC", L"演出包", SS_LEFT, kIdShowLabel);
    state->show_edit = MakeControl(state, L"EDIT", L"",
                                   WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kIdShowEdit);
    state->show_browse = MakeControl(state, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
                                     kIdShowBrowse);
    state->show_play = MakeControl(state, L"BUTTON", L"播放演出", BS_PUSHBUTTON | WS_TABSTOP,
                                   kIdShowPlay);

    state->clear_log = MakeControl(state, L"BUTTON", L"清空日志", BS_PUSHBUTTON | WS_TABSTOP,
                                   kIdClearLog);
    state->log = MakeControl(state, L"EDIT", L"",
                             WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                 ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                             kIdLog);
    if (state->log != nullptr && state->mono != nullptr) {
        SendMessageW(state->log, WM_SETFONT, reinterpret_cast<WPARAM>(state->mono), TRUE);
    }
}

void LoadConfigIntoControls(AppState* state) {
    Config config;
    LoadConfig(&config);
    SetText(state->dir_edit, config.game_dir);
    SetText(state->key_edit, std::wstring(config.key_hex.begin(), config.key_hex.end()));
    SetText(state->bundle_edit, config.bundle_path);

    // 演出包没配置时，默认用可执行文件旁边的 sng001.lvs，这样双击打开就能直接播。
    std::wstring show_path = config.show_path;
    if (show_path.empty()) {
        wchar_t exe[MAX_PATH * 2] = {0};
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        if (length > 0 && length < MAX_PATH * 2) {
            std::wstring candidate(exe, length);
            const size_t slash = candidate.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                candidate.resize(slash + 1);
            }
            candidate += L"sng001.lvs";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                show_path = candidate;
            }
        }
    }
    SetText(state->show_edit, show_path);
    if (!config.game_dir.empty() || !config.key_hex.empty()) {
        AppendLog(state, L"已读取上次的目录与密钥，可直接点「校验 pak 索引」");
    }
}

void DestroyState(HWND window);

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    AppState* state = StateOf(window);

    switch (message) {
        case WM_CREATE: {
            auto* created = new AppState();
            created->window = window;
            created->background = CreateSolidBrush(RGB(240, 240, 240));
            created->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            created->mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                        FIXED_PITCH | FF_MODERN, L"Consolas");
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            CreateControls(created);

            RECT client{};
            GetClientRect(window, &client);
            LayoutControls(created, client.right - client.left, client.bottom - client.top);
            LoadConfigIntoControls(created);
            AppendLog(created, L"选择游戏资源目录、填入 AES 密钥，然后点「校验 pak 索引」。");
            // 已经配好演出包时，打开软件就自动开始播放（延迟一点等窗口画完）
            if (!GetText(created->show_edit).empty()) {
                SetTimer(window, 1, 600, nullptr);
            }
            return 0;
        }
        case WM_SIZE: {
            if (state != nullptr) {
                LayoutControls(state, LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 760;
            info->ptMinTrackSize.y = 620;
            return 0;
        }
        case WM_TIMER: {
            if (wparam == 1) {
                KillTimer(window, 1);
                if (state != nullptr && !GetText(state->show_edit).empty()) {
                    PostMessageW(window, WM_COMMAND,
                                 MAKEWPARAM(kIdShowPlay, BN_CLICKED), 0);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            if (state == nullptr) {
                return 0;
            }
            // 只处理按钮点击（BN_CLICKED == 0），忽略编辑框的通知
            if (HIWORD(wparam) != BN_CLICKED) {
                return 0;
            }
            OnCommand(state, LOWORD(wparam));
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND control = reinterpret_cast<HWND>(lparam);
            // 日志框保持系统配色，其余标签/分组框跟随窗口底色
            if (state != nullptr && GetDlgCtrlID(control) != kIdLog) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkColor(dc, RGB(240, 240, 240));
                return reinterpret_cast<LRESULT>(state->background);
            }
            break;
        }
        case WM_DESTROY: {
            if (state != nullptr) {
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_NCDESTROY: {
            // 窗口真正销毁后释放状态（含 GDI 对象）
            DestroyState(window);
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void DestroyState(HWND window) {
    AppState* state = StateOf(window);
    if (state == nullptr) {
        return;
    }
    if (state->mono != nullptr) {
        DeleteObject(state->mono);
    }
    if (state->background != nullptr) {
        DeleteObject(state->background);
    }
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
}

}  // namespace

int RunAppWindow() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    window_class.lpszClassName = L"liveview_main_window";
    if (RegisterClassExW(&window_class) == 0) {
        CoUninitialize();
        return 1;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect = {0, 0, 980, 700};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    HWND window = CreateWindowExW(
        0, L"liveview_main_window", L"liveview —— 演出资源验证工具", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
        nullptr, instance, nullptr);
    if (window == nullptr) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    CoUninitialize();
    return 0;
}

}  // namespace liveview
