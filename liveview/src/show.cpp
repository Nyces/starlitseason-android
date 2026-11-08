// 演出包读取与时间轴采样（实现）。

#include "show.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace liveview {
namespace {

// 顶点结构在磁盘上是紧排的：3+3+3 个 float，再 4 个 uint16 + 4 个 float。
// x86/x64 为小端，与包内字节序一致，可以直接整块读。
static_assert(sizeof(ShowVertex) == 36, "ShowVertex 必须是 36 字节");
static_assert(sizeof(ShowSkinVertex) == 60, "ShowSkinVertex 必须是 60 字节");

constexpr char kMagic[4] = {'L', 'V', 'S', '2'};
constexpr uint32_t kVersion = 2;

struct Reader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    bool ok = true;

    size_t Remaining() const { return ok && at <= size ? size - at : 0; }

    bool Need(size_t count) {
        if (!ok || count > Remaining()) {
            ok = false;
            return false;
        }
        return true;
    }

    template <typename T>
    T Scalar() {
        T value{};
        if (!Need(sizeof(T))) {
            return value;
        }
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    uint8_t U8() { return Scalar<uint8_t>(); }
    uint16_t U16() { return Scalar<uint16_t>(); }
    uint32_t U32() { return Scalar<uint32_t>(); }
    int32_t I32() { return Scalar<int32_t>(); }
    int64_t I64() { return Scalar<int64_t>(); }
    float F32() { return Scalar<float>(); }
    double F64() { return Scalar<double>(); }

    bool Bytes(void* out, size_t count) {
        if (!Need(count)) {
            return false;
        }
        std::memcpy(out, data + at, count);
        at += count;
        return true;
    }

    std::string String() {
        const uint16_t length = U16();
        if (!Need(length)) {
            return std::string();
        }
        std::string text(reinterpret_cast<const char*>(data + at), length);
        at += length;
        return text;
    }
};

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out, std::string* error) {
    std::FILE* file = _wfopen(path.c_str(), L"rb");
    if (file == nullptr) {
        *error = "无法打开演出包";
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        *error = "读取演出包长度失败";
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0) {
        std::fclose(file);
        *error = "读取演出包长度失败";
        return false;
    }
    std::rewind(file);
    out->resize(static_cast<size_t>(length));
    const size_t read = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), file);
    std::fclose(file);
    if (read != out->size()) {
        *error = "演出包读取不完整";
        return false;
    }
    return true;
}

void QuatSlerp(const float* a, const float* b, float t, float* out) {
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    const float dot = a[0] * bx + a[1] * by + a[2] * bz + a[3] * bw;
    if (dot < 0.0f) {   // 走短弧
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    const float cos_half = std::fabs(dot);
    float scale0 = 0.0f;
    float scale1 = 0.0f;
    if (cos_half > 0.9995f) {   // 太接近，直接线性插值再归一化
        scale0 = 1.0f - t;
        scale1 = t;
    } else {
        const float angle = std::acos(cos_half);
        const float sin_angle = std::sin(angle);
        scale0 = std::sin((1.0f - t) * angle) / sin_angle;
        scale1 = std::sin(t * angle) / sin_angle;
    }
    float x = a[0] * scale0 + bx * scale1;
    float y = a[1] * scale0 + by * scale1;
    float z = a[2] * scale0 + bz * scale1;
    float w = a[3] * scale0 + bw * scale1;
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 1e-8f) {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    } else {
        x = 0.0f;
        y = 0.0f;
        z = 0.0f;
        w = 1.0f;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

}  // namespace

bool Show::Load(const std::wstring& path, std::string* error) {
    std::vector<uint8_t> data;
    if (!ReadWholeFile(path, &data, error)) {
        return false;
    }
    Reader reader{data.data(), data.size(), 0, true};

    char magic[4] = {};
    if (!reader.Bytes(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
        *error = "不是演出包（magic 不匹配）";
        return false;
    }
    const uint32_t version = reader.U32();
    if (version != kVersion) {
        *error = "演出包版本不支持：" + std::to_string(version);
        return false;
    }

    camera_tick_fps = reader.F64();
    anim_fps = reader.F64();
    duration = reader.F64();
    character_offset[0] = reader.F32();
    character_offset[1] = reader.F32();
    character_offset[2] = reader.F32();

    const uint32_t camera_count = reader.U32();
    if (!reader.ok || camera_count > reader.Remaining() / 32) {
        *error = "相机关键帧数量异常";
        return false;
    }
    camera_keys.resize(camera_count);
    for (ShowCameraKey& key : camera_keys) {
        key.frame = reader.I64();
        for (float& value : key.position) {
            value = reader.F32();
        }
        for (float& value : key.target) {
            value = reader.F32();
        }
    }

    const uint32_t stage_count = reader.U32();
    if (!reader.ok || stage_count > reader.Remaining() / 8) {
        *error = "舞台网格数量异常";
        return false;
    }
    stages.resize(stage_count);
    for (ShowStage& stage : stages) {
        const uint32_t vertex_count = reader.U32();
        if (!reader.ok || vertex_count > reader.Remaining() / sizeof(ShowVertex)) {
            *error = "舞台顶点数量异常";
            return false;
        }
        stage.vertices.resize(vertex_count);
        if (vertex_count > 0 &&
            !reader.Bytes(stage.vertices.data(), vertex_count * sizeof(ShowVertex))) {
            *error = "舞台顶点读取失败";
            return false;
        }
        const uint32_t index_count = reader.U32();
        if (!reader.ok || index_count > reader.Remaining() / 4) {
            *error = "舞台索引数量异常";
            return false;
        }
        stage.indices.resize(index_count);
        if (index_count > 0 &&
            !reader.Bytes(stage.indices.data(), index_count * sizeof(uint32_t))) {
            *error = "舞台索引读取失败";
            return false;
        }
    }

    const uint32_t skin_count = reader.U32();
    if (!reader.ok || skin_count > reader.Remaining() / 8) {
        *error = "蒙皮网格数量异常";
        return false;
    }
    skins.resize(skin_count);
    for (ShowSkin& skin : skins) {
        skin.attach_bone = reader.String();
        const uint32_t bone_count = reader.U32();
        if (!reader.ok || bone_count > reader.Remaining() / 44) {
            *error = "骨骼数量异常";
            return false;
        }
        skin.bones.resize(bone_count);
        for (ShowBone& bone : skin.bones) {
            bone.name = reader.String();
            bone.parent = reader.I32();
            for (float& value : bone.rotation) {
                value = reader.F32();
            }
            for (float& value : bone.translation) {
                value = reader.F32();
            }
            for (float& value : bone.scale) {
                value = reader.F32();
            }
        }
        const uint32_t vertex_count = reader.U32();
        if (!reader.ok || vertex_count > reader.Remaining() / sizeof(ShowSkinVertex)) {
            *error = "蒙皮顶点数量异常";
            return false;
        }
        skin.vertices.resize(vertex_count);
        if (vertex_count > 0 &&
            !reader.Bytes(skin.vertices.data(), vertex_count * sizeof(ShowSkinVertex))) {
            *error = "蒙皮顶点读取失败";
            return false;
        }
        const uint32_t index_count = reader.U32();
        if (!reader.ok || index_count > reader.Remaining() / 4) {
            *error = "蒙皮索引数量异常";
            return false;
        }
        skin.indices.resize(index_count);
        if (index_count > 0 &&
            !reader.Bytes(skin.indices.data(), index_count * sizeof(uint32_t))) {
            *error = "蒙皮索引读取失败";
            return false;
        }
    }

    const uint32_t bone_count = reader.U32();
    const uint32_t pose_count = reader.U32();
    if (!reader.ok || bone_count > reader.Remaining() / 2) {
        *error = "动画骨骼数量异常";
        return false;
    }
    anim_bones.resize(bone_count);
    for (std::string& name : anim_bones) {
        name = reader.String();
    }
    if (!reader.ok || pose_count > reader.Remaining() / 8) {
        *error = "动画帧数量异常";
        return false;
    }
    poses.resize(pose_count);
    for (ShowPose& pose : poses) {
        pose.frame = reader.I64();
        pose.quats.resize(static_cast<size_t>(bone_count) * 4);
        pose.has_rotation.resize(bone_count);
        pose.has_translation.resize(bone_count);
        pose.translations.resize(static_cast<size_t>(bone_count) * 3);
        for (uint32_t i = 0; i < bone_count; ++i) {
            for (int component = 0; component < 4; ++component) {
                pose.quats[static_cast<size_t>(i) * 4 + component] = reader.F32();
            }
            pose.has_rotation[i] = reader.U8();
            pose.has_translation[i] = reader.U8();
            for (int component = 0; component < 3; ++component) {
                pose.translations[static_cast<size_t>(i) * 3 + component] =
                    pose.has_translation[i] ? reader.F32() : 0.0f;
            }
        }
    }

    if (!reader.ok) {
        *error = "演出包数据不完整";
        return false;
    }
    if (reader.Remaining() != 0) {
        *error = "演出包尾部有未识别数据";
        return false;
    }
    return true;
}

ShowCameraPose Show::SampleCamera(double seconds) const {
    ShowCameraPose pose;
    if (camera_keys.empty()) {
        return pose;
    }
    const double frame = seconds * camera_tick_fps;
    if (frame <= static_cast<double>(camera_keys.front().frame)) {
        std::memcpy(pose.position, camera_keys.front().position, sizeof(pose.position));
        std::memcpy(pose.target, camera_keys.front().target, sizeof(pose.target));
        return pose;
    }
    if (frame >= static_cast<double>(camera_keys.back().frame)) {
        std::memcpy(pose.position, camera_keys.back().position, sizeof(pose.position));
        std::memcpy(pose.target, camera_keys.back().target, sizeof(pose.target));
        return pose;
    }

    size_t lo = 0;
    size_t hi = camera_keys.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (static_cast<double>(camera_keys[mid].frame) <= frame) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const double span = static_cast<double>(camera_keys[hi].frame - camera_keys[lo].frame);
    const double t = span > 0.0 ? (frame - static_cast<double>(camera_keys[lo].frame)) / span
                                : 0.0;
    for (int i = 0; i < 3; ++i) {
        pose.position[i] = static_cast<float>(
            camera_keys[lo].position[i] +
            (camera_keys[hi].position[i] - camera_keys[lo].position[i]) * t);
        pose.target[i] = static_cast<float>(
            camera_keys[lo].target[i] +
            (camera_keys[hi].target[i] - camera_keys[lo].target[i]) * t);
    }
    return pose;
}

std::vector<ShowBonePose> Show::SamplePose(double seconds) const {
    std::vector<ShowBonePose> out(anim_bones.size());
    if (poses.empty() || anim_bones.empty()) {
        return out;
    }
    const double frame = seconds * anim_fps;

    size_t lo = 0;
    size_t hi = 0;
    if (frame <= static_cast<double>(poses.front().frame)) {
        lo = hi = 0;
    } else if (frame >= static_cast<double>(poses.back().frame)) {
        lo = hi = poses.size() - 1;
    } else {
        hi = poses.size() - 1;
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (static_cast<double>(poses[mid].frame) <= frame) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
    }
    const double span = static_cast<double>(poses[hi].frame - poses[lo].frame);
    const float t = (hi != lo && span > 0.0)
                        ? static_cast<float>((frame - static_cast<double>(poses[lo].frame)) / span)
                        : 0.0f;

    const ShowPose& a = poses[lo];
    const ShowPose& b = poses[hi];
    const size_t bone_count = anim_bones.size();
    for (size_t i = 0; i < bone_count; ++i) {
        ShowBonePose& bone = out[i];
        const float* qa = &a.quats[i * 4];
        const float* qb = &b.quats[i * 4];
        const bool ra = a.has_rotation[i] != 0;
        const bool rb = b.has_rotation[i] != 0;
        if (ra && rb) {
            QuatSlerp(qa, qb, t, bone.rotation);
            bone.has_rotation = true;
        } else if (ra) {
            std::copy(qa, qa + 4, bone.rotation);
            bone.has_rotation = true;
        } else if (rb) {
            std::copy(qb, qb + 4, bone.rotation);
            bone.has_rotation = true;
        }
        const float* ta = &a.translations[i * 3];
        const float* tb = &b.translations[i * 3];
        const bool ha = a.has_translation[i] != 0;
        const bool hb = b.has_translation[i] != 0;
        if (ha && hb && t > 0.0f) {
            for (int c = 0; c < 3; ++c) {
                bone.translation[c] = ta[c] + (tb[c] - ta[c]) * t;
            }
            bone.has_translation = true;
        } else if (ha) {
            std::copy(ta, ta + 3, bone.translation);
            bone.has_translation = true;
        } else if (hb) {
            std::copy(tb, tb + 3, bone.translation);
            bone.has_translation = true;
        }
    }
    return out;
}

size_t Show::StageVertexCount() const {
    size_t total = 0;
    for (const ShowStage& stage : stages) {
        total += stage.vertices.size();
    }
    return total;
}

size_t Show::StageTriangleCount() const {
    size_t total = 0;
    for (const ShowStage& stage : stages) {
        total += stage.indices.size() / 3;
    }
    return total;
}

size_t Show::SkinVertexCount() const {
    size_t total = 0;
    for (const ShowSkin& skin : skins) {
        total += skin.vertices.size();
    }
    return total;
}

size_t Show::SkinTriangleCount() const {
    size_t total = 0;
    for (const ShowSkin& skin : skins) {
        total += skin.indices.size() / 3;
    }
    return total;
}

}  // namespace liveview
