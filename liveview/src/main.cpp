// liveview —— live 演出验证程序
//
// 里程碑 1（原生）：
//   双击启动 → 选游戏资源目录 → 输入 AES 密钥（记住到本机配置）→
//   原生解析 pak 索引并校验（AES-256 / UE4 pak v8）。
//   仍然支持以前的 bundle 命令：--at / --play / --render。
// 后续里程碑：原生解析资产（骨骼网格 / 动画 / 材质）、舞台与角色渲染、音轨同步。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
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
#include "config.h"
#include "dialogs.h"
#include "pak.h"

#ifdef LIVEVIEW_RENDER
#include "render_vulkan.h"
#endif

namespace {

void PrintUsage() {
    std::printf(
        "用法:\n"
        "  liveview                     双击模式：选目录、输密钥、校验 pak\n"
        "  liveview --launcher          同上\n"
        "  liveview --paks <目录> --key <64位十六进制>   命令行校验 pak\n"
        "  liveview <bundle>            打印包信息与首末关键帧\n"
        "  liveview <bundle> --at <帧>  输出该帧插值后的机位\n"
        "  liveview <bundle> --play <秒> 按 tick 频率逐帧推进并打印机位\n"
        "  liveview <bundle> --render[=秒] 打开窗口按时间轴播放（需编译渲染后端）\n");
}

void PrintPose(const liveview::Bundle& bundle, double frame) {
    const liveview::CameraKey pose = liveview::SampleCamera(bundle, frame);
    std::printf("帧 %lld  机位 %.2f %.2f %.2f  瞄准 %.2f %.2f %.2f\n",
                static_cast<long long>(frame), pose.position[0], pose.position[1],
                pose.position[2], pose.target[0], pose.target[1], pose.target[2]);
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

// UTF-8 窄串 -> UTF-16（中文字符串给 MessageBox 用）
std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), length);
    return out;
}

std::vector<std::wstring> FindPaks(const std::wstring& directory) {
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW((directory + L"\\*.pak").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return paths;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::wstring name = data.cFileName;
        std::wstring lower = name;
        for (wchar_t& ch : lower) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        // 与 Python 侧一致：默认跳过 add/free/lyric/song 这几类扩展包
        if (lower.rfind(L"add", 0) == 0 || lower.rfind(L"free", 0) == 0 ||
            lower.rfind(L"lyric", 0) == 0 || lower.rfind(L"song", 0) == 0) {
            continue;
        }
        paths.push_back(directory + L"\\" + name);
    } while (FindNextFileW(handle, &data) != 0);
    FindClose(handle);
    return paths;
}

// 校验目录下所有 pak 的索引；返回是否至少有一个成功。summary 给 UI 显示。
bool CheckPaks(const std::wstring& directory, const std::string& key_hex, std::wstring* summary,
               std::string* error, int* ok_paks = nullptr, int64_t* total_entries = nullptr) {
    std::vector<uint8_t> key;
    if (!liveview::ParseHexKey(key_hex, &key)) {
        *error = "AES 密钥必须是 64 位十六进制";
        return false;
    }
    const std::vector<std::wstring> paths = FindPaks(directory);
    if (paths.empty()) {
        *error = "目录下没有找到 .pak 文件";
        return false;
    }

    std::wstring text;
    int64_t entries_total = 0;
    int ok_count = 0;
    for (const std::wstring& path : paths) {
        liveview::PakInfo info;
        std::string pak_error;
        if (!liveview::LoadPakIndex(path, key, &info, &pak_error)) {
            std::printf("  [FAIL] %s: %s\n", Narrow(path).c_str(), pak_error.c_str());
            text += L"  [FAIL] " + path + L": " + Widen(pak_error) + L"\n";
            continue;
        }
        ++ok_count;
        entries_total += static_cast<int64_t>(info.entries.size());
        std::printf("  %s  version=%d entries=%lld mount=%s\n", Narrow(path).c_str(),
                    static_cast<int>(info.version),
                    static_cast<long long>(info.entries.size()), info.mount_point.c_str());
        text += L"  " + path.substr(directory.size() + 1) + L"  v" +
                std::to_wstring(info.version) + L"  条目 " +
                std::to_wstring(info.entries.size()) + L"\n";
    }

    *summary = L"游戏目录：" + directory + L"\n\n" + text + L"\n共 " +
               std::to_wstring(ok_count) + L" / " + std::to_wstring(paths.size()) +
               L" 个 pak 索引解析成功，条目合计 " + std::to_wstring(entries_total) + L" 条。";
    if (ok_paks != nullptr) {
        *ok_paks = ok_count;
    }
    if (total_entries != nullptr) {
        *total_entries = entries_total;
    }
    return ok_count > 0;
}

int RunLauncher() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    liveview::Config config;
    liveview::LoadConfig(&config);
    std::string error;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (config.game_dir.empty()) {
            std::wstring folder;
            if (!liveview::PickGameFolder(&folder)) {
                CoUninitialize();
                return 1;
            }
            config.game_dir = folder;
        }
        std::vector<uint8_t> key;
        if (!liveview::ParseHexKey(config.key_hex, &key)) {
            std::string entered;
            if (!liveview::AskAesKey(&entered, config.key_hex)) {
                CoUninitialize();
                return 1;
            }
            config.key_hex = entered;
        }

        std::wstring summary;
        if (CheckPaks(config.game_dir, config.key_hex, &summary, &error)) {
            liveview::SaveConfig(config);
            MessageBoxW(nullptr, summary.c_str(), L"liveview —— pak 校验通过",
                        MB_OK | MB_ICONINFORMATION);
            CoUninitialize();
            return 0;
        }
        MessageBoxW(nullptr, (L"校验失败：\n" + Widen(error) +
                              L"\n\n请重新选择目录或密钥。").c_str(),
                    L"liveview", MB_OK | MB_ICONWARNING);
        config.game_dir.clear();
        config.key_hex.clear();
    }
    CoUninitialize();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return RunLauncher();
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        PrintUsage();
        return 0;
    }
    if (std::strcmp(argv[1], "--launcher") == 0) {
        return RunLauncher();
    }
    if (std::strcmp(argv[1], "--paks") == 0) {
        std::wstring directory;
        std::string key_hex;
        if (argc < 3) {
            PrintUsage();
            return 1;
        }
        const std::string narrow = argv[2];
        directory.assign(narrow.begin(), narrow.end());
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--key" && i + 1 < argc) {
                key_hex = argv[++i];
            } else {
                PrintUsage();
                return 1;
            }
        }
        if (directory.empty() || key_hex.empty()) {
            PrintUsage();
            return 1;
        }
        std::wstring summary;
        std::string error;
        int ok_paks = 0;
        int64_t entries_total = 0;
        const bool ok = CheckPaks(directory, key_hex, &summary, &error, &ok_paks, &entries_total);
        if (!ok) {
            std::fprintf(stderr, "pak check failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("\n%d pak(s) OK, %lld entries total\n", ok_paks,
                    static_cast<long long>(entries_total));
        return 0;
    }

    const std::string path = argv[1];
    liveview::Bundle bundle;
    std::string error;
    if (!liveview::LoadBundle(path, &bundle, &error)) {
        std::fprintf(stderr, "读取失败: %s\n", error.c_str());
        return 1;
    }

    std::printf("包信息\n");
    std::printf("  资源      %s\n", bundle.asset.c_str());
    std::printf("  相机绑定  %s\n", bundle.binding.c_str());
    std::printf("  tick      %.3f fps\n", bundle.tick_fps);
    std::printf("  范围      %lld .. %lld（步长 %lld）\n",
                static_cast<long long>(bundle.range_start),
                static_cast<long long>(bundle.range_end),
                static_cast<long long>(bundle.range_step));
    std::printf("  关键帧    %zu 个\n", bundle.keys.size());

    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--at" && i + 1 < argc) {
            PrintPose(bundle, std::atof(argv[++i]));
        } else if (option == "--play" && i + 1 < argc) {
            const double seconds = std::atof(argv[++i]);
            if (bundle.tick_fps <= 0.0) {
                std::fprintf(stderr, "包内 tick 非法，无法播放\n");
                return 1;
            }
            const int64_t total = static_cast<int64_t>(seconds * bundle.tick_fps);
            const int64_t display_step = static_cast<int64_t>(bundle.tick_fps) / 60;
            if (display_step <= 0) {
                std::fprintf(stderr, "tick 太低，无法按 60 帧输出\n");
                return 1;
            }
            for (int64_t frame = 0; frame <= total; frame += display_step) {
                PrintPose(bundle, static_cast<double>(frame));
            }
        } else if (option == "--render" || option.rfind("--render=", 0) == 0) {
#ifdef LIVEVIEW_RENDER
            double seconds = -1.0;
            if (option.size() > 9) {
                seconds = std::atof(option.c_str() + 9);
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                seconds = std::atof(argv[++i]);
            }
            return liveview::RunRender(bundle, seconds);
#else
            std::fprintf(stderr,
                         "本构建未启用渲染后端（需用 -DLIVEVIEW_RENDER=ON 重新构建）\n");
            return 1;
#endif
        } else {
            PrintUsage();
            return 1;
        }
    }
    return 0;
}
