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
    float uv[2] = {0.0f, 0.0f};
};

// 绑定姿势顶点 + 最多 4 个骨骼影响。
struct ShowSkinVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float uv[2] = {0.0f, 0.0f};
    uint16_t bones[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// 解压后的贴图（RGBA8，行序自上而下）。
struct ShowTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// 一个材质的 Toon 参数（与 Python 预览器的 SceneItem 同义）。
struct ShowMaterial {
    int32_t base_texture = -1;
    int32_t sphere_texture = -1;
    int32_t shadow_texture = -1;
    float shadow_tint[3] = {1.0f, 1.0f, 1.0f};
    float threshold = 0.47f;
    float blend[3] = {1.0f, 1.0f, 1.0f};
    float add[3] = {0.0f, 0.0f, 0.0f};
    float rim_scale = 7.5f;
    float sphere_blend[3] = {1.0f, 1.0f, 1.0f};
    float sphere_pow = 1.0f;
    float sphere_boost = 1.0f;
    float outline_width = 0.0f;   // >0 = 描边壳，顶点沿法线外扩
    uint8_t blend_mode = 0;   // 0 opaque / 1 masked / 2 additive / 3 translucent
    bool unlit = false;
    bool outline = false;     // 网格里的外扩描边壳，渲染时换个剔除顺序
};

// 网格里的一个材质分段：索引区间 + 材质下标。
struct ShowSection {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t material = -1;
};

struct ShowStage {
    std::vector<ShowVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ShowSection> sections;
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
    std::vector<ShowSection> sections;
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
    std::vector<ShowTexture> textures;
    std::vector<ShowMaterial> materials;
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
