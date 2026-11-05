// liveview —— live 演出验证程序（骨架）
//
// 当前能力：
//   读取 live bundle、按 tick 采样相机、以时间轴方式逐帧推进（--play）。
// 后续里程碑：Vulkan 渲染后端、舞台与角色数据接入、音轨同步。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bundle.h"

namespace {

void PrintUsage() {
    std::printf(
        "用法:\n"
        "  liveview <bundle>            打印包信息与首末关键帧\n"
        "  liveview <bundle> --at <帧>  输出该帧插值后的机位\n"
        "  liveview <bundle> --play <秒> 按 tick 频率逐帧推进并打印机位\n");
}

void PrintPose(const liveview::Bundle& bundle, double frame) {
    const liveview::CameraKey pose = liveview::SampleCamera(bundle, frame);
    std::printf("帧 %lld  机位 %.2f %.2f %.2f  瞄准 %.2f %.2f %.2f\n",
                static_cast<long long>(frame), pose.position[0], pose.position[1],
                pose.position[2], pose.target[0], pose.target[1], pose.target[2]);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        PrintUsage();
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
        } else {
            PrintUsage();
            return 1;
        }
    }
    return 0;
}
