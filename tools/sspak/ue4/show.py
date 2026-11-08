"""导出 liveview 演出包（二进制小端）。

包内容：相机轨 + 舞台静态网格 + 角色蒙皮网格 + 骨骼动画采样。
所有资产解析、蒙皮权重映射与动画语义都留在 Python 侧；C++ 侧只做读取、
CPU 线性混合蒙皮与渲染。

坐标与矩阵约定：与 Python 渲染器一致，矩阵按列向量左乘（p' = M · p）。
舞台网格在导出时已把放置变换烘进顶点，C++ 不需要再乘模型矩阵。

格式（全部小端）：

    char[4] "LVS2"
    u32     version = 2

    --- 时间轴 ---
    f64 camera_tick_fps        // 相机轨的 tick 频率
    f64 anim_fps               // 动画采样频率（通常 60）
    f64 duration               // 演出时长（秒）
    f32 character_offset[3]    // 角色整体落位偏移（脚底对齐舞台地面）
    u32 camera_key_count
        key_count × { i64 frame; f32 position[3]; f32 target[3] }

    --- 舞台 ---
    u32 stage_count
        stage_count × {
            u32 vertex_count
                vertex_count × { f32 position[3]; f32 normal[3]; f32 color[3] }
            u32 index_count; index_count × u32
        }

    --- 蒙皮网格 ---
    u32 skin_count
        skin_count × {
            u16 attach_len; char attach[attach_len]   // 空 = 不跟随；否则为骨骼名（如 SO_Head）
            u32 bone_count
                bone_count × { u16 name_len; char name[name_len];
                               i32 parent; f32 quat[4]; f32 translation[3]; f32 scale[3] }
            u32 vertex_count
                vertex_count × { f32 position[3]; f32 normal[3]; f32 color[3];
                                 u16 bone_index[4]; f32 weight[4] }
            u32 index_count; index_count × u32
        }

    --- 动画（所有蒙皮网格共用同一套骨骼名） ---
    u32 animated_bone_count
    u32 pose_count
    animated_bone_count × u16 name_len; char name[name_len]
    pose_count × {
        i64 frame
        animated_bone_count × { f32 quat[4]; u8 has_rotation; u8 has_translation;
                                有位移则 f32 translation[3] }
    }
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

SHOW_MAGIC = b"LVS2"
SHOW_VERSION = 2


def quat_from_matrix(matrix: np.ndarray) -> tuple[float, float, float, float]:
    """3×3 旋转矩阵 -> 四元数 (x, y, z, w)。"""
    m = np.asarray(matrix, dtype=np.float64)
    trace = float(m[0, 0] + m[1, 1] + m[2, 2])
    if trace > 0.0:
        s = float(np.sqrt(trace + 1.0)) * 2.0
        w = 0.25 * s
        x = float(m[2, 1] - m[1, 2]) / s
        y = float(m[0, 2] - m[2, 0]) / s
        z = float(m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = float(np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2])) * 2.0
        w = float(m[2, 1] - m[1, 2]) / s
        x = 0.25 * s
        y = float(m[0, 1] + m[1, 0]) / s
        z = float(m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = float(np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2])) * 2.0
        w = float(m[0, 2] - m[2, 0]) / s
        x = float(m[0, 1] + m[1, 0]) / s
        y = 0.25 * s
        z = float(m[1, 2] + m[2, 1]) / s
    else:
        s = float(np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1])) * 2.0
        w = float(m[1, 0] - m[0, 1]) / s
        x = float(m[0, 2] + m[2, 0]) / s
        y = float(m[1, 2] + m[2, 1]) / s
        z = 0.25 * s
    norm = float(np.sqrt(x * x + y * y + z * z + w * w))
    if norm <= 1e-12:
        return 0.0, 0.0, 0.0, 1.0
    return x / norm, y / norm, z / norm, w / norm


@dataclass
class StageMesh:
    positions: np.ndarray            # (N,3) float32，世界坐标
    normals: np.ndarray              # (N,3) float32
    colors: np.ndarray               # (N,3) float32
    indices: np.ndarray              # (M,) uint32


@dataclass
class SkinMesh:
    bones: list[tuple[str, int]]     # (名字, 父索引)
    bind_pose: np.ndarray            # (B,10) 四元数 xyzw + 平移 + 缩放
    positions: np.ndarray            # (N,3) 绑定姿势
    normals: np.ndarray              # (N,3)
    colors: np.ndarray               # (N,3) float32
    bone_indices: np.ndarray         # (N,4) uint16
    weights: np.ndarray              # (N,4) float32
    indices: np.ndarray              # (M,) uint32
    attach_bone: str = ""            # 非空时整块网格跟随该骨骼（头发/饰品）


@dataclass
class Animation:
    bone_names: list[str]            # 被动画的骨骼名
    frames: list[int]
    quats: np.ndarray                # (poses, bones, 4) float32
    has_rotation: np.ndarray         # (poses, bones) bool
    translations: np.ndarray         # (poses, bones, 3) float32，缺位移处为 NaN


class ShowWriter:
    """按上面的格式顺序写出演出包。"""

    def __init__(self, path: str):
        self._file = open(path, "wb")
        self._file.write(SHOW_MAGIC)
        self._u32(SHOW_VERSION)

    # ---- 基础写入 ----

    def _raw(self, data: bytes) -> None:
        self._file.write(data)

    def _u8(self, value: int) -> None:
        self._raw(struct.pack("<B", value))

    def _u16(self, value: int) -> None:
        self._raw(struct.pack("<H", value))

    def _u32(self, value: int) -> None:
        self._raw(struct.pack("<I", value))

    def _i32(self, value: int) -> None:
        self._raw(struct.pack("<i", value))

    def _i64(self, value: int) -> None:
        self._raw(struct.pack("<q", value))

    def _f32(self, value: float) -> None:
        self._raw(struct.pack("<f", value))

    def _f64(self, value: float) -> None:
        self._raw(struct.pack("<d", value))

    def _f32v(self, values) -> None:
        array = np.asarray(values, dtype="<f4").reshape(-1)
        self._raw(array.tobytes())

    def _u32v(self, values) -> None:
        array = np.asarray(values, dtype="<u4").reshape(-1)
        self._raw(array.tobytes())

    def _string(self, text: str) -> None:
        encoded = text.encode("utf-8")
        if len(encoded) > 65535:
            raise ValueError("名字过长")
        self._u16(len(encoded))
        self._raw(encoded)

    # ---- 分段写入 ----

    def timeline(self, camera_tick_fps: float, anim_fps: float, duration: float,
                 character_offset) -> None:
        self._f64(camera_tick_fps)
        self._f64(anim_fps)
        self._f64(duration)
        self._f32v(character_offset)

    def camera(self, keys) -> None:
        self._u32(len(keys))
        for frame, position, target in keys:
            self._i64(frame)
            self._f32v(position)
            self._f32v(target)

    def stage(self, meshes: list[StageMesh]) -> None:
        self._u32(len(meshes))
        for mesh in meshes:
            count = len(mesh.positions)
            self._u32(count)
            packed = np.hstack([
                np.asarray(mesh.positions, dtype=np.float32).reshape(count, 3),
                np.asarray(mesh.normals, dtype=np.float32).reshape(count, 3),
                np.asarray(mesh.colors, dtype=np.float32).reshape(count, 3),
            ]).astype("<f4")
            self._raw(packed.tobytes())
            self._u32(len(mesh.indices))
            self._u32v(mesh.indices)

    def skins(self, meshes: list[SkinMesh]) -> None:
        self._u32(len(meshes))
        for mesh in meshes:
            self._string(mesh.attach_bone)
            self._u32(len(mesh.bones))
            for index, (name, parent) in enumerate(mesh.bones):
                self._string(name)
                self._i32(parent)
                self._f32v(mesh.bind_pose[index][:4])
                self._f32v(mesh.bind_pose[index][4:7])
                self._f32v(mesh.bind_pose[index][7:10])
            count = len(mesh.positions)
            self._u32(count)
            for i in range(count):
                self._f32v(mesh.positions[i])
                self._f32v(mesh.normals[i])
                self._f32v(mesh.colors[i])
                self._raw(np.asarray(mesh.bone_indices[i], dtype="<u2").tobytes())
                self._f32v(mesh.weights[i])
            self._u32(len(mesh.indices))
            self._u32v(mesh.indices)

    def animation(self, animation: Animation) -> None:
        bones = len(animation.bone_names)
        self._u32(bones)
        self._u32(len(animation.frames))
        for name in animation.bone_names:
            self._string(name)
        for pose_index, frame in enumerate(animation.frames):
            self._i64(frame)
            for bone in range(bones):
                self._f32v(animation.quats[pose_index, bone])
                self._u8(1 if animation.has_rotation[pose_index, bone] else 0)
                translation = animation.translations[pose_index, bone]
                if np.all(np.isnan(translation)):
                    self._u8(0)
                else:
                    self._u8(1)
                    self._f32v(translation)

    def close(self) -> None:
        self._file.close()
