// liveview —— 演出资源验证工具
//
// 默认双击启动图形界面（所有参数都在界面里设置）。
// 命令行入口保留给 CI 与排查用。
//
// 里程碑 1：pak 索引解析（AES-256-ECB / UE4 pak v8）。
// 里程碑 2：条目解压（zlib）+ UE4 cooked 包解析（名称表 / 导入导出 / 属性标签）。
// 后续：骨骼网格与动画解析、Vulkan 渲染舞台与角色、音轨同步。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "aes256.h"
#include "app_window.h"
#include "bundle.h"
#include "commands.h"
#include "pak.h"

#ifdef LIVEVIEW_RENDER
#include "render_vulkan.h"
#endif

namespace {

void PrintUsage() {
    std::printf(
        "用法（不带参数时启动图形界面）:\n"
        "  liveview                                   图形界面\n"
        "  liveview --selftest                        自检（AES 测试向量 + 密钥解析）\n"
        "  liveview --paks <目录> --key <密钥> [命令]  命令行操作\n"
        "      （默认）校验索引；--list [关键字] 列条目；\n"
        "      --extract <条目名> <输出文件> 导出条目；--dump <资产路径> 打印包结构\n"
        "  liveview <bundle>                          打印包信息与首末关键帧\n"
        "  liveview <bundle> --at <帧>                输出该帧插值后的机位\n"
        "  liveview <bundle> --play <秒>              按 tick 频率逐帧推进并打印机位\n"
        "  liveview <bundle> --render[=秒]            打开窗口按时间轴播放（需渲染后端）\n");
}

void PrintPose(const liveview::Bundle& bundle, double frame) {
    const liveview::CameraKey pose = liveview::SampleCamera(bundle, frame);
    std::printf("帧 %lld  机位 %.2f %.2f %.2f  瞄准 %.2f %.2f %.2f\n",
                static_cast<long long>(frame), pose.position[0], pose.position[1],
                pose.position[2], pose.target[0], pose.target[1], pose.target[2]);
}

// 命令行参数是 ANSI（CP_ACP），转成宽串以支持中文路径。
std::wstring FromAnsi(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_ACP, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}

liveview::LogSink PrintSink() {
    return [](const std::string& line) { std::printf("%s\n", line.c_str()); };
}

// 自检：AES-256 按 FIPS-197 测试向量核对，密钥解析做正反例。
// CI 上没有游戏数据，用它保证密码学部分没有回归。
int RunSelfTest() {
    const std::string hex = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    std::vector<uint8_t> key;
    if (!liveview::ParseHexKey(hex, &key) || key.size() != 32) {
        std::fprintf(stderr, "自检失败: 密钥解析\n");
        return 1;
    }
    const uint8_t cipher[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
                                0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    const uint8_t expected[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t plain[16];
    {
        uint8_t block[16];
        std::memcpy(block, cipher, 16);
        uint8_t round_keys[240];
        liveview::Aes256ExpandKey(key.data(), round_keys);
        liveview::Aes256DecryptBlock(round_keys, block, plain);
    }
    if (std::memcmp(plain, expected, 16) != 0) {
        std::fprintf(stderr, "自检失败: AES-256 测试向量\n");
        return 1;
    }
    // 反例：长度不对、含非十六进制字符都必须被拒
    std::vector<uint8_t> bad;
    if (liveview::ParseHexKey("1234", &bad) || liveview::ParseHexKey(std::string(64, 'z'), &bad)) {
        std::fprintf(stderr, "自检失败: 非法密钥未被拒绝\n");
        return 1;
    }
    std::printf("自检通过：AES-256 测试向量 + 密钥解析\n");
    return 0;
}

int RunPaksCommand(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }
    const std::wstring directory = FromAnsi(argv[2]);
    std::string key_hex;
    std::string command = "check";
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--key" && i + 1 < argc) {
            key_hex = argv[++i];
        } else if (option == "--list") {
            command = "list";
        } else if (option == "--extract") {
            command = "extract";
        } else if (option == "--dump") {
            command = "dump";
        } else {
            rest.push_back(option);
        }
    }

    std::vector<uint8_t> key;
    if (!liveview::ParseHexKey(key_hex, &key)) {
        std::fprintf(stderr, "密钥必须是 64 位十六进制\n");
        return 1;
    }

    std::vector<liveview::PakInfo> paks;
    std::string error;
    if (!liveview::LoadAllPaks(directory, key, &paks, PrintSink(), &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    if (command == "list") {
        liveview::ListEntries(paks, rest.empty() ? std::string() : rest[0], 0, PrintSink());
        return 0;
    }
    if (command == "extract") {
        if (rest.size() < 2) {
            PrintUsage();
            return 1;
        }
        if (!liveview::ExtractEntry(paks, key, rest[0], FromAnsi(rest[1]), PrintSink(), &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if (command == "dump") {
        if (rest.empty()) {
            PrintUsage();
            return 1;
        }
        if (!liveview::DumpAsset(paks, key, rest[0], PrintSink(), &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    liveview::SummarizePaks(paks, PrintSink());
    return 0;
}

int RunBundleCommand(int argc, char** argv) {
    liveview::Bundle bundle;
    std::string error;
    if (!liveview::LoadBundleW(FromAnsi(argv[1]), &bundle, &error)) {
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
            std::fprintf(stderr, "本构建未启用渲染后端（需用 -DLIVEVIEW_RENDER=ON 重新构建）\n");
            return 1;
#endif
        } else {
            PrintUsage();
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        // 双击启动时顺手关掉附带的控制台窗口，只留图形界面
        FreeConsole();
        return liveview::RunAppWindow();
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        PrintUsage();
        return 0;
    }
    if (std::strcmp(argv[1], "--launcher") == 0 || std::strcmp(argv[1], "--gui") == 0) {
        return liveview::RunAppWindow();
    }
    if (std::strcmp(argv[1], "--selftest") == 0) {
        return RunSelfTest();
    }
    if (std::strcmp(argv[1], "--paks") == 0) {
        return RunPaksCommand(argc, argv);
    }
    return RunBundleCommand(argc, argv);
}
