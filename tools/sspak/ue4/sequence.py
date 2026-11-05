"""UE4 LevelSequence（MovieScene）解析：绑定、轨道、区段与帧范围。

目前覆盖：
    LevelSequence -> MovieScene
    Possessables（角色/特效占位）
    ObjectBindings -> Tracks
    Track -> Sections -> SectionRange
    PlaybackRange / TickResolution

关键帧通道（MovieSceneFloatChannel 的 Times/Values）逐帧解码还在验证中，
这里只记录每个区段包含的通道数与默认值，避免把没验证的数据当成结果。
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

from .package import Package
from .properties import Property, array_items, parse_properties


class SequenceError(RuntimeError):
    pass


@dataclass
class Possessable:
    name: str
    guid: bytes
    class_path: str
    parent_guid: bytes = b""


@dataclass
class Section:
    export_index: int            # 1 起，0 表示未解析到
    class_name: str
    start: int | None            # None = Open（该侧没有边界）
    end: int | None
    # 通道名 -> 关键帧 [(帧号, 值)]，只解析浮点通道
    channels: dict[str, list[tuple[int, float]]] = field(default_factory=dict)


@dataclass
class Track:
    export_index: int
    class_name: str
    sections: list[Section] = field(default_factory=list)


@dataclass
class Binding:
    name: str
    guid: bytes
    tracks: list[Track] = field(default_factory=list)


@dataclass
class MovieSceneInfo:
    possessables: list[Possessable] = field(default_factory=list)
    bindings: list[Binding] = field(default_factory=list)
    play_start: int | None = None
    play_end: int | None = None
    tick_fps: float = 0.0
    display_fps: float = 0.0


def _resolve_export_index(pkg: Package, index: int) -> int:
    """对象索引（正数=导出 1 起，负数=导入）只关心导出。"""
    return index if index > 0 else 0


def _text(pkg: Package, data: bytes, prop: Property) -> str:
    if prop.type == "StrProperty":
        # StrProperty 的值是 [int32 长度][UTF-8/NUL]，长度包含结尾的 0
        raw = prop.value(data)
        if len(raw) < 4:
            return ""
        text = raw[4:].split(b"\x00", 1)[0]
        return text.decode("utf-8", "replace")
    if prop.type == "NameProperty":
        index = struct.unpack_from("<i", data, prop.offset)[0]
        return pkg.names[index] if 0 <= index < len(pkg.names) else ""
    if prop.type == "ObjectProperty":
        index = struct.unpack_from("<i", data, prop.offset)[0]
        return _resolve_path(pkg, index)
    return ""


def _resolve_path(pkg: Package, index: int) -> str:
    parts: list[str] = []
    cur = index
    for _ in range(20):
        if cur < 0:
            imp = pkg.imports[-cur - 1]
            parts.append(imp.object_name)
            cur = imp.outer_index
        elif cur > 0:
            export = pkg.exports[cur - 1]
            parts.append(export.name)
            cur = export.outer_index
        else:
            break
    return "/".join(reversed(parts))


def _frame_range(data: bytes, prop: Property) -> tuple[int | None, int | None]:
    """TRange<FFrameNumber>：下界类型(1) + 下界(4) + 上界类型(1) + 上界(4)。

    类型 2 表示 Open（该侧没有值），这时对应帧号记成 None。
    """
    lower_type = data[prop.offset]
    upper_type = data[prop.offset + 5]
    start = None if lower_type == 2 else struct.unpack_from("<i", data, prop.offset + 1)[0]
    end = None if upper_type == 2 else struct.unpack_from("<i", data, prop.offset + 6)[0]
    return start, end


def _struct_fields(data: bytes, names: list[str], prop: Property) -> list[Property]:
    """读取结构体属性的内部字段流。"""
    inner, _ = parse_properties(data, names, prop.offset)
    return inner


# MovieSceneFloatChannel 的 Times / Values 数组：计数(4) + 结构体标签头(49) + 元素。
TIMES_HEADER = 49
TIMES_STRIDE = 4            # FFrameNumber: int32
VALUES_STRIDE = 23          # float 值(4) + 三个模式字节(3) + 四个切线(16)


def _float_channel(data: bytes, names: list[str], prop: Property) -> list[tuple[int, float]]:
    """解出 MovieSceneFloatChannel 的关键帧 [(帧号, 值)]。"""
    inner, _ = parse_properties(data, names, prop.offset)
    times = next((p for p in inner if p.name == "Times"), None)
    values = next((p for p in inner if p.name == "Values"), None)
    if times is None or values is None:
        return []
    count = struct.unpack_from("<i", data, times.offset)[0]
    if count <= 0:
        return []
    if times.size < 4 + TIMES_HEADER + count * TIMES_STRIDE:
        return []
    if values.size < 4 + TIMES_HEADER + count * VALUES_STRIDE:
        return []
    times_at = times.offset + 4 + TIMES_HEADER
    values_at = values.offset + 4 + TIMES_HEADER
    keys = []
    for i in range(count):
        frame = struct.unpack_from("<i", data, times_at + i * TIMES_STRIDE)[0]
        value = struct.unpack_from("<f", data, values_at + i * VALUES_STRIDE)[0]
        keys.append((int(frame), float(value)))
    return keys


def _collect_channels(data: bytes, names: list[str], prop: Property,
                      out: dict[str, list[tuple[int, float]]]) -> None:
    """递归收集一个结构体里的浮点通道。"""
    if prop.type != "StructProperty":
        return
    struct_name = prop.struct_name or ""
    if struct_name == "MovieSceneFloatChannel":
        out[prop.name] = _float_channel(data, names, prop)
        return
    # 通道可能包在自定义结构里（LiveCameraMovementSection 的 Channels）
    try:
        inner, _ = parse_properties(data, names, prop.offset)
    except Exception:  # noqa: BLE001 - 非标签结构体
        return
    for child in inner:
        _collect_channels(data, names, child, out)


def _section(pkg: Package, export_index: int) -> Section:
    export = pkg.exports[export_index - 1]
    data = pkg.export_bytes(export)
    props, _ = parse_properties(data, pkg.names)
    start: int | None = None
    end: int | None = None
    for prop in props:
        if prop.name == "SectionRange":
            start, end = _frame_range(data, prop)
    section = Section(export_index=export_index, class_name=export.class_name,
                      start=start, end=end)
    for prop in props:
        if prop.type != "StructProperty":
            continue
        if prop.name in ("Easing", "SectionRange", "Signature"):
            continue
        _collect_channels(data, pkg.names, prop, section.channels)
    return section


def _track(pkg: Package, export_index: int) -> Track:
    export = pkg.exports[export_index - 1]
    data = pkg.export_bytes(export)
    props, _ = parse_properties(data, pkg.names)
    track = Track(export_index=export_index, class_name=export.class_name)
    for prop in props:
        if prop.name != "Sections" or prop.type != "ArrayProperty":
            continue
        count = struct.unpack_from("<i", data, prop.offset)[0]
        for i in range(count):
            index = struct.unpack_from("<i", data, prop.offset + 4 + i * 4)[0]
            if index <= 0:
                continue
            track.sections.append(_section(pkg, index))
    return track


def sample_channel(keys: list[tuple[int, float]], frame: float) -> float | None:
    """在关键帧之间线性插值；超出两端时钳制。"""
    if not keys:
        return None
    if frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    for i in range(1, len(keys)):
        right_frame, right_value = keys[i]
        if frame <= right_frame:
            left_frame, left_value = keys[i - 1]
            span = right_frame - left_frame
            if span <= 0:
                return right_value
            t = (frame - left_frame) / span
            return left_value + (right_value - left_value) * t
    return keys[-1][1]


def sample_section(section: Section, frame: float) -> dict[str, float]:
    """取某个区段在指定帧的通道值。"""
    out: dict[str, float] = {}
    for name, keys in section.channels.items():
        value = sample_channel(keys, frame)
        if value is not None:
            out[name] = value
    return out


@dataclass
class CameraPose:
    position: tuple[float, float, float]
    target: tuple[float, float, float]


def camera_pose(binding: Binding, frame: float) -> CameraPose | None:
    """把 LiveCameraMovement 的通道还原成机位。

    实测约定（已在 Stg111 舞台上渲出正确画面）：
        FreeLocationX/Y/Z 是环绕中心点，
        RotateOffsetDistance 是相机到中心点的距离，
        RotateOffsetYaw/Pitch 是环绕方位角（度，绕 Z 轴 / 俯仰）。
    """
    track = next((t for t in binding.tracks
                  if t.class_name == "LiveCameraMovementTrack" and t.sections), None)
    if track is None:
        return None
    values = sample_section(track.sections[0], frame)
    centre = (values.get("FreeLocationX", 0.0),
              values.get("FreeLocationY", 0.0),
              values.get("FreeLocationZ", 0.0))
    distance = values.get("RotateOffsetDistance", 0.0)
    yaw = math.radians(values.get("RotateOffsetYaw", 0.0))
    pitch = math.radians(values.get("RotateOffsetPitch", 0.0))
    offset = (math.sin(yaw) * math.cos(pitch),
              math.cos(yaw) * math.cos(pitch),
              math.sin(pitch))
    position = (centre[0] + offset[0] * distance,
                centre[1] + offset[1] * distance,
                centre[2] + offset[2] * distance)
    return CameraPose(position=position, target=centre)


def parse_level_sequence(package: Package) -> MovieSceneInfo:
    scene_export = package.find_export("LevelSequence")
    if scene_export is None:
        scene_export = package.find_export("MovieSceneSequence")
    if scene_export is None:
        raise SequenceError("该资源不是 LevelSequence")
    data = package.export_bytes(scene_export)
    props, _ = parse_properties(data, package.names)

    info = MovieSceneInfo()
    movie_index = 0
    for prop in props:
        if prop.name == "MovieScene" and prop.type == "ObjectProperty":
            movie_index = struct.unpack_from("<i", data, prop.offset)[0]

    movie = None
    if movie_index > 0:
        export = package.exports[movie_index - 1]
        if export.class_name != "MovieScene":
            movie = next((e for e in package.exports if e.class_name == "MovieScene"), None)
    if movie is None:
        movie = next((e for e in package.exports if e.class_name == "MovieScene"), None)
    if movie is None:
        raise SequenceError("找不到 MovieScene")
    data = package.export_bytes(movie)
    props, _ = parse_properties(data, package.names)

    for prop in props:
        if prop.name == "Possessables":
            for item in array_items(data, package.names, prop):
                fields = {p.name: p for p in item}
                guid = fields.get("Guid")
                name = fields.get("Name")
                cls = fields.get("PossessedObjectClass")
                info.possessables.append(Possessable(
                    name=_text(package, data, name) if name else "",
                    guid=data[guid.offset:guid.offset + 16] if guid else b"",
                    class_path=_text(package, data, cls) if cls else "",
                ))
        elif prop.name == "ObjectBindings":
            for item in array_items(data, package.names, prop):
                fields = {p.name: p for p in item}
                guid = fields.get("ObjectGuid")
                name = fields.get("BindingName")
                tracks = fields.get("Tracks")
                binding = Binding(
                    name=_text(package, data, name) if name else "",
                    guid=data[guid.offset:guid.offset + 16] if guid else b"",
                )
                if tracks is not None:
                    count = struct.unpack_from("<i", data, tracks.offset)[0]
                    for i in range(count):
                        index = struct.unpack_from("<i", data, tracks.offset + 4 + i * 4)[0]
                        if 1 <= index <= len(package.exports):
                            binding.tracks.append(_track(package, index))
                info.bindings.append(binding)
        elif prop.name == "PlaybackRange":
            info.play_start, info.play_end = _frame_range(data, prop)
        elif prop.name in ("TickResolution", "DisplayRate"):
            # FrameRate 是带标签的结构体，内部有 Numerator / Denominator
            numerator, denominator = 0, 1
            if prop.struct_name == "FrameRate":
                for inner in _struct_fields(data, package.names, prop):
                    value = struct.unpack_from("<i", data, inner.offset)[0]
                    if inner.name == "Numerator":
                        numerator = value
                    elif inner.name == "Denominator":
                        denominator = value
            fps = numerator / denominator if denominator else 0.0
            if prop.name == "TickResolution":
                info.tick_fps = fps
            else:
                info.display_fps = fps
    return info
