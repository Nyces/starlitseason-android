// 演出包（*.lvs）读取与时间轴采样。
//
// 包由 `sspak live-show` 生成，二进制小端。Python 侧负责解析 pak、蒙皮权重、
// 动画语义与舞台摆放；这里只做读取、时间轴插值与 CPU 蒙皮所需的数据准备。
//
// 格式见 tools/sspak/ue4/show.py 顶部注释。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace liveview {

// 世界坐标的顶点（舞台已在导出时烘好放置变换）。
struct ShowVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float color[3] = {1.0f, 1.0f, 1.0f};
};

// 绑定姿势顶点 + 最多 4 个骨骼影响。
struct ShowSkinVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float color[3] = {1.0f, 1.0f, 1.0f};
    uint16_t bones[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct ShowStage {
    std::vector<ShowVertex> vertices;
    std::vector<uint32_t> indices;
};

struct ShowBone {
    std::string name;
    int32_t parent = -1;
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // 四元数 (x, y, z, w)
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct ShowSkin {
    std::string attach_bone;                          // 非空：整块网格跟随该骨骼（头发/饰品）
    std::vector<ShowBone> bones;
    std::vector<ShowSkinVertex> vertices;
    std::vector<uint32_t> indices;
};

struct ShowCameraKey {
    int64_t frame = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
};

struct ShowPose {
    int64_t frame = 0;
    std::vector<float> quats;              // 每骨骼 4 个分量
    std::vector<uint8_t> has_rotation;     // 每骨骼 1 字节
    std::vector<uint8_t> has_translation;  // 每骨骼 1 字节
    std::vector<float> translations;       // 每骨骼 3 个分量
};

// 采样后的单根骨骼姿势。
struct ShowBonePose {
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool has_rotation = false;
    float translation[3] = {0.0f, 0.0f, 0.0f};
    bool has_translation = false;
};

struct ShowCameraPose {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
};

struct Show {
    double camera_tick_fps = 0.0;
    double anim_fps = 60.0;
    double duration = 0.0;
    float character_offset[3] = {0.0f, 0.0f, 0.0f};

    std::vector<ShowCameraKey> camera_keys;
    std::vector<ShowStage> stages;
    std::vector<ShowSkin> skins;
    std::vector<std::string> anim_bones;
    std::vector<ShowPose> poses;

    // 读取演出包；失败时返回 false 并填充 error。
    bool Load(const std::wstring& path, std::string* error);

    // 时间（秒）-> 相机机位（关键帧之间线性插值）。
    ShowCameraPose SampleCamera(double seconds) const;

    // 时间（秒）-> 每根动画骨骼的姿势（四元数 slerp）。
    std::vector<ShowBonePose> SamplePose(double seconds) const;

    // 各段统计（给命令行/日志用）。
    size_t StageVertexCount() const;
    size_t StageTriangleCount() const;
    size_t SkinVertexCount() const;
    size_t SkinTriangleCount() const;
};

}  // namespace liveview
