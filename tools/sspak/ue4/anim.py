"""UE4.24 cooked AnimSequence：解出每根骨骼的关键帧。

布局是**靠样本反推**的实验结论（零售包没有文档），解析带校验：
旋转块必须全部落在单位球内（四元数 xyz 只存三个分量），否则该轨判为
不可用并丢弃——宁可保留绑定姿势，也不把别的通道当成旋转画出来。

实测布局（A_sng001_D_Pos01，NumFrames=8393，60fps）：
    FPropertyTag 属性流（NumFrames / TrackToSkeletonMapTable / SequenceLength / Skeleton）
    int32 0, FGuid(16), 10 字节标志
    TArray<FTrackToSkeletonMap>：int32 数量 + 每项 int32 骨骼下标（主骨架）
    int32 0, int32 65536
    int32 NumFrames（同时也是整条轨的键数）
    int32 组数，紧跟 组数 × 4 个 int32：每轨 [旋转键数, 旋转结束偏移, 位移键数, 位移结束偏移]
    键流：每个键 12 字节（3×float），偏移相对键流起点

**偏移是「结束位置」而不是起点**：每轨的数据块是 [旋转键][位移键] 首尾相接，
表里记的是各自通道的结束字节位置，所以键起点 = 结束偏移 - 键数×12。
按这个规则，125 个块在整条键流里严丝合缝地铺满（0 处不连续），这是判定
布局正确的硬证据；旧实现把偏移当起点再 +24，等于把每根骨头读成了「下一根
骨头」的曲线——姿势还是人形，但舞蹈完全不是原版。

单键通道（键数 1）表示「整段恒定」，读出来不是合法四元数时丢弃、回退绑定姿势。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .package import Package
from .properties import parse_properties

CURVE_TAIL = b"/Engine/Animation/DefaultAnimCurveCompressionSettings"
KEY_SIZE = 12
QUAT_TOLERANCE = 1.001


class AnimSequenceError(RuntimeError):
    pass


@dataclass
class AnimSequence:
    name: str
    num_frames: int
    sequence_length: float
    bone_indices: np.ndarray                  # 轨道 -> 主骨架骨骼下标
    rotations: list[np.ndarray] = field(default_factory=list)   # 每轨 (K,3) 四元数 xyz
    positions: list[np.ndarray] = field(default_factory=list)   # 每轨 (K,3) 位移
    quality: float = 0.0                     # 旋转通道的自检通过率
    dropped: int = 0                         # 判为不可用而丢弃的轨道数

    def sample(self, frame: float, channels: str = "rot") -> dict[int, np.ndarray]:
        """取某一帧：返回 {主骨架骨骼下标: 3×3 旋转矩阵}。"""
        out: dict[int, np.ndarray] = {}
        keys = self.rotations if channels == "rot" else self.positions
        for i, bone in enumerate(self.bone_indices):
            if i >= len(keys) or keys[i] is None or len(keys[i]) == 0:
                continue
            v = _sample_keys(keys[i], frame, self.num_frames)
            out[int(bone)] = v
        return out

    def sample_translations(self, frame: float) -> dict[int, np.ndarray]:
        out: dict[int, np.ndarray] = {}
        for i, bone in enumerate(self.bone_indices):
            if i >= len(self.positions) or self.positions[i] is None:
                continue
            if len(self.positions[i]) == 0:
                continue
            out[int(bone)] = _sample_keys(self.positions[i], frame, self.num_frames)
        return out


def _sample_keys(keys: np.ndarray, frame: float, num_frames: int) -> np.ndarray:
    """按帧号取键。键在整段上均匀分布（恒定为 1 键时直接返回）。"""
    count = len(keys)
    if count == 1:
        return keys[0]
    t = float(np.clip(frame, 0.0, max(num_frames - 1, 1))) * (count - 1) / max(num_frames - 1, 1)
    i0 = int(np.floor(t))
    i1 = min(i0 + 1, count - 1)
    blend = t - i0
    return keys[i0] * (1.0 - blend) + keys[i1] * blend


def quat_matrix(xyz) -> np.ndarray:
    """(x, y, z) 三分量 + 正 W 还原四元数，再转 3×3 矩阵。

    只存 xyz 时 w = sqrt(1 - x² - y² - z²)；浮点误差可能让根号下的值略小于 0，
    这里钳制到 0 并归一化，保证输出始终是正交矩阵。
    """
    x, y, z = (float(v) for v in xyz)
    length = float(np.sqrt(x * x + y * y + z * z))
    if length > 1.0:
        x, y, z = x / length, y / length, z / length
        length = 1.0
    w = float(np.sqrt(max(1.0 - length * length, 0.0)))
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def parse_animation(package: Package) -> AnimSequence:
    export = package.find_export("AnimSequence")
    if export is None:
        raise AnimSequenceError("该资源不包含 AnimSequence")
    data = package.export_bytes(export)
    props, props_end = parse_properties(data, package.names)
    num_frames = next((struct.unpack_from("<i", data, p.offset)[0]
                       for p in props if p.name == "NumFrames"), 0)
    length = next((struct.unpack_from("<f", data, p.offset)[0]
                   for p in props if p.name == "SequenceLength"), 0.0)
    table = next((p for p in props if p.name == "TrackToSkeletonMapTable"), None)
    if table is None or num_frames <= 0:
        raise AnimSequenceError("缺少 NumFrames / 轨道表")

    # 属性区的轨道表：结构体数组，第一项带完整标签（49 字节），之后每项 37 字节
    track_count = struct.unpack_from("<i", data, table.offset)[0]
    bone_indices = []
    for i in range(track_count):
        if i == 0:
            value_at = table.offset + 4 + 49 + 25
        else:
            value_at = table.offset + 4 + 86 + (i - 1) * 37 + 25
        bone_indices.append(struct.unpack_from("<i", data, value_at)[0])
    bone_indices = np.array(bone_indices, dtype=np.int32)

    pos = props_end
    pos += 4 + 16 + 10                       # int32 0 + FGuid + 10 字节标志
    map_count = struct.unpack_from("<i", data, pos)[0]
    if map_count != track_count:
        raise AnimSequenceError(f"轨道映射数量不符 {map_count} != {track_count}")
    pos += 4 + map_count * 4
    pos += 8                                 # int32 0, int32 65536
    key_count = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    if not 0 < key_count <= 1 << 20:
        raise AnimSequenceError(f"整条轨的键数异常 {key_count}")
    group_count = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    if group_count <= 0 or group_count % 4 != 0:
        raise AnimSequenceError(f"轨道偏移表长度异常 {group_count}")
    groups = np.frombuffer(data, dtype="<i4", count=group_count,
                           offset=pos).astype(np.int64).reshape(-1, 4)
    stream_start = pos + group_count * 4

    tail = data.find(CURVE_TAIL)
    if tail < 0:
        raise AnimSequenceError("找不到曲线名字符串，无法定位键流尾部")
    stream_end = tail - 4
    if stream_end <= stream_start:
        raise AnimSequenceError("键流为空")
    stream = data[stream_start:stream_end]

    seq = AnimSequence(export.name, num_frames, float(length), bone_indices)
    valid_rot = 0
    rot_blocks = 0
    for row in groups:
        rot_count, rot_offset, pos_count, pos_offset = (int(v) for v in row)
        rotation = _read_rotation_block(stream, rot_count, rot_offset, key_count)
        repaired = None if rotation is None else _repair_rotations(rotation)
        seq.rotations.append(repaired if repaired is not None
                             else np.zeros((0, 3), np.float32))
        if rot_count > 1:
            # 自检只看逐帧变化的旋转通道：恒定通道的偏移不可信，不参与统计
            rot_blocks += 1
            if repaired is not None:
                valid_rot += 1
            else:
                seq.dropped += 1
        translation = _read_block(stream, pos_count, pos_offset, key_count)
        if translation is None:
            translation = np.zeros((0, 3), np.float32)
        seq.positions.append(translation)
    seq.quality = valid_rot / rot_blocks if rot_blocks else 0.0
    return seq


def _read_block(stream: bytes, count: int, end_offset: int, key_count: int) -> np.ndarray | None:
    """读一条通道的键；偏移是结束位置，数量/偏移不合法时返回 None。"""
    if count <= 0 or count > key_count:
        return None
    offset = end_offset - count * KEY_SIZE
    end = end_offset
    if offset < 0 or end > len(stream):
        return None
    return np.frombuffer(stream, dtype="<f4", count=count * 3,
                         offset=offset).reshape(-1, 3)


def _read_rotation_block(stream: bytes, count: int, end_offset: int,
                         key_count: int) -> np.ndarray | None:
    """读旋转块：与位移块同样的「结束偏移」规则，没有额外压缩头。"""
    return _read_block(stream, count, end_offset, key_count)


def _repair_rotations(keys: np.ndarray) -> np.ndarray | None:
    """校验并修复旋转键。

    四元数只存 xyz，必须满足 x²+y²+z² ≤ 1。全键块里平均有 0.01% 的键越界
    （压缩器的量化误差/边界），逐键替换成上一个合法键，避免那一帧把骨骼甩飞。
    越界键超过 5% 说明这条轨道本身不可用（偏移指进了别的通道），返回 None。
    """
    if len(keys) == 0:
        return None
    norms = np.linalg.norm(keys.astype(np.float64), axis=1)
    bad = norms > QUAT_TOLERANCE
    if not np.any(bad):
        return keys
    if float(np.mean(bad)) > 0.05:
        return None
    fixed = keys.copy()
    last = np.zeros(3, np.float32)
    for i in range(len(fixed)):
        if bad[i]:
            fixed[i] = last
        else:
            last = fixed[i]
    # 开头若是坏键，用第一个合法键回填
    first_valid = int(np.argmax(~bad))
    if bad[:first_valid].any():
        fixed[:first_valid] = fixed[first_valid]
    return fixed
