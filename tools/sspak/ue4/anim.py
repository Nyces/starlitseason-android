"""UE4.24 cooked AnimSequence：解出每根骨骼的关键帧。

布局是**靠样本反推**的实验结论（旧版 ConstantKeyLerp 的序列化在零售包里
没有文档），因此解析带校验：任何一步对不上就抛 AnimSequenceError，
调用方可以退回绑定姿势，避免把错误的姿势画出来。

实测布局：
    FPropertyTag 属性流（NumFrames / SequenceLength / TrackToSkeletonMapTable）
    int32 0, FGuid(16), 10 字节标志
    TArray<FTrackToSkeletonMap>：int32 数量 + 每项 int32 骨骼下标（主骨架）
    int32 0, int32 65536
    int32 NumKeys
    TArray<int32> 轨道偏移
    紧跟着的第二段偏移（缩放通道）
    int32 0
    FByteBulkData 字节流：每个键 12 字节（3×float）

键的语义：半径 `(x, y, z)`，旋转把 w 用 `sqrt(1 - x² - y² - z²)` 补回；
偏移表按 (键数, 字节偏移) 成对出现，排序后每个轨占 channels 个连续通道。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .package import Package
from .properties import parse_properties

CURVE_TAIL = b"/Engine/Animation/DefaultAnimCurveCompressionSettings"
KEY_SIZE = 12


class AnimSequenceError(RuntimeError):
    pass


@dataclass
class AnimSequence:
    name: str
    num_frames: int
    sequence_length: float
    bone_indices: np.ndarray                  # 轨道 -> 主骨架骨骼下标
    rotations: list[np.ndarray] = field(default_factory=list)   # 每轨 (K,3)
    positions: list[np.ndarray] = field(default_factory=list)   # 每轨 (K,3)，可能为空
    scales: list[np.ndarray] = field(default_factory=list)
    quality: float = 0.0                     # 实验解析的自检通过率

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

    实验解析偶尔会把位移键当成旋转键，分量可能超出单位球；这里先归一化，
    保证输出的始终是正交矩阵（不会把蒙皮矩阵变成奇异矩阵）。
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
    num_keys = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    if not 0 < num_keys <= 4096:
        raise AnimSequenceError(f"键数量异常 {num_keys}")
    offset_count = struct.unpack_from("<i", data, pos)[0]
    pos += 4
    if offset_count <= 0 or offset_count > 4096:
        raise AnimSequenceError(f"偏移表异常 {offset_count}")
    offsets = np.frombuffer(data, dtype="<i4", count=offset_count, offset=pos).astype(np.int64)
    pos += offset_count * 4

    tail = data.find(CURVE_TAIL)
    if tail < 0:
        raise AnimSequenceError("找不到曲线名字符串，无法定位字节流尾部")
    stream_end = tail - 4

    extra = np.frombuffer(data, dtype="<i4", count=16, offset=pos).astype(np.int64)
    variants = [
        offsets[2:],
        np.concatenate([offsets[2:], extra]),
        offsets,
        np.concatenate([offsets, extra]),
    ]
    best = None
    for values in variants:
        pairs = _collect_pairs(values, num_keys)
        if len(pairs) < track_count:
            continue
        max_end = max(off + count * KEY_SIZE for count, off in pairs)
        base = stream_end - max_end
        if base < pos + 4 or base > stream_end:
            continue
        pairs.sort(key=lambda item: item[1])
        channels = min(max(len(pairs) // track_count, 2), 3)
        for rot_channel in (1, 0):
            score = _score_rotations(pairs, base, data, track_count, channels, rot_channel)
            if best is None or score > best[0]:
                best = (score, pairs, base, channels, rot_channel)
    if best is None:
        raise AnimSequenceError("无法定位字节流起点")
    score, pairs, base, channels, rot_channel = best
    if score < 0.6:
        raise AnimSequenceError(f"旋转键校验通过率过低（{score:.2f}）")

    seq = AnimSequence(export.name, num_frames, float(length), bone_indices,
                       quality=score)
    for t in range(track_count):
        rotations = positions = None
        for c in range(channels):
            index = t * channels + c
            if index >= len(pairs):
                break
            count, off = pairs[index]
            keys = _read_keys(data, base + off, count)
            if c == rot_channel:
                rotations = keys
            elif c == (1 - rot_channel) and c < 2:
                positions = keys
        seq.rotations.append(rotations if _valid_rotations(rotations)
                             else np.zeros((0, 3), np.float32))
        seq.positions.append(positions if positions is not None
                             else np.zeros((0, 3), np.float32))
    return seq


def _collect_pairs(values: np.ndarray, num_keys: int) -> list[tuple[int, int]]:
    """把整数流解释成 (键数, 字节偏移) 对，丢弃不合法的组合。"""
    pairs = []
    for i in range(0, len(values) - 1, 2):
        count, off = int(values[i]), int(values[i + 1])
        if 1 <= count <= num_keys and off >= 0:
            pairs.append((count, off))
    return pairs


def _valid_rotations(keys: np.ndarray | None) -> bool:
    """旋转键的三个分量必须落在单位球内（w 靠 sqrt 补回）。"""
    if keys is None or len(keys) == 0:
        return False
    return bool(np.all(np.isfinite(keys)) and np.all(np.abs(keys) <= 1.02))


def _score_rotations(pairs, base, data, track_count, channels, rot_channel) -> float:
    good = checked = 0
    for t in range(track_count):
        index = t * channels + rot_channel
        if index >= len(pairs):
            continue
        count, off = pairs[index]
        try:
            keys = _read_keys(data, base + off, count)
        except AnimSequenceError:
            continue
        checked += 1
        if _valid_rotations(keys):
            good += 1
    return good / checked if checked else 0.0


def _read_keys(data: bytes, offset: int, count: int) -> np.ndarray:
    end = offset + count * KEY_SIZE
    if end > len(data):
        raise AnimSequenceError("键数据越界")
    return np.frombuffer(data, dtype="<f4", count=count * 3, offset=offset).reshape(-1, 3)
