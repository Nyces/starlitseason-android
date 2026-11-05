// 文本包（live bundle）读取。
//
// 包由 `sspak live-bundle` 生成，逐行文本：
//   #liveview-bundle 1        版本
//   #asset <路径>             来源序列资源
//   #binding <名字>           使用的相机绑定
//   #tick <fps>               序列的 tick 频率（帧/秒）
//   #range <起> <止> <步长>    导出范围
//   F <帧> <px> <py> <pz> <tx> <ty> <tz>   机位与瞄准点（厘米）
// 以 # 开头的行都是注释。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace liveview {

struct CameraKey {
    int64_t frame = 0;
    double position[3] = {0.0, 0.0, 0.0};
    double target[3] = {0.0, 0.0, 0.0};
};

struct Bundle {
    std::string asset;
    std::string binding;
    double tick_fps = 0.0;
    int64_t range_start = 0;
    int64_t range_end = 0;
    int64_t range_step = 1;
    std::vector<CameraKey> keys;
};

// 读取文本包；失败时返回 false 并填充 error。
bool LoadBundle(const std::string& path, Bundle* bundle, std::string* error);

// 按帧号线性插值；超出两端时钳制到端点。
CameraKey SampleCamera(const Bundle& bundle, double frame);

}  // namespace liveview
