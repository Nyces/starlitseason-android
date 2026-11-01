"""UE4.24 StaticMesh：属性流 + LOD0 几何（位置/索引/分段）。

当前只解出渲染验证所需的最小集合：
- 跳过 TaggedProperties（StructProperty 带 16 字节 StructGuid）
- 定位 cooked 原生数据起点
- 读取 LOD0 的 FPositionVertexBuffer 与首个 FRawStaticIndexBuffer
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np


class StaticMeshError(RuntimeError):
    pass


@dataclass
class MeshSection:
    material_index: int
    first_index: int
    num_triangles: int
    min_vertex: int
    max_vertex: int


@dataclass
class StaticMesh:
    positions: np.ndarray
    indices: np.ndarray
    sections: list[MeshSection] = field(default_factory=list)


def _fname(buf: bytes, pos: int) -> tuple[tuple[int, int], int]:
    index, number = struct.unpack_from("<ii", buf, pos)
    return (index, number), pos + 8


def _skip_properties(data: bytes, names: list[str], start: int = 0) -> int:
    """跳过 TaggedProperties，返回终止 None 标签之后的位置。"""
    pos = start
    for _ in range(4096):
        (index, number), p = _fname(data, pos)
        name = names[index] if 0 <= index < len(names) else f"<{index}>"
        if name == "None" and number == 0:
            return p
        (type_index, _tn), p = _fname(data, pos + 8)
        prop_type = names[type_index] if 0 <= type_index < len(names) else f"<{type_index}>"
        size, _array_index = struct.unpack_from("<ii", data, pos + 16)
        p = pos + 24
        if prop_type == "StructProperty":
            _sn, p = _fname(data, p)
            p += 16  # StructGuid
        elif prop_type == "BoolProperty":
            p += 1
        elif prop_type in ("ByteProperty", "EnumProperty"):
            _en, p = _fname(data, p)
        elif prop_type in ("ArrayProperty", "SetProperty", "OptionalProperty"):
            _it, p = _fname(data, p)
        elif prop_type == "MapProperty":
            _it, p = _fname(data, p)
            _vt, p = _fname(data, p)
        has_guid = data[p]
        p += 1
        if has_guid:
            p += 16
        pos = p + size
    raise StaticMeshError("属性流未找到 None 终止符")


def _i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def _find_native_start(data: bytes, start: int) -> int:
    """在属性流之后定位 UStaticMesh 原生数据起点。

    cooked 数据布局（4.24）：
      FStripDataFlags(2) bCooked(4) BodySetup(4) NavCollision(4)
      LightingGuid(16) SocketsCount(4) LODCount(4)
    """
    for offset in range(start, min(start + 128, len(data) - 64)):
        if data[offset] != 1 or data[offset + 1] != 0:
            continue
        if _i32(data, offset + 2) != 1:
            continue
        body = _i32(data, offset + 6)
        nav = _i32(data, offset + 10)
        sockets = _i32(data, offset + 30)
        lods = _i32(data, offset + 34)
        if 0 <= body <= 8 and 0 <= nav <= 8 and sockets == 0 and 1 <= lods <= 8:
            return offset
    raise StaticMeshError("未找到 StaticMesh 原生数据起点")


def parse_static_mesh(export_data: bytes, names: list[str]) -> StaticMesh:
    props_end = _skip_properties(export_data, names)
    pos = _find_native_start(export_data, props_end)
    pos += 2 + 4 + 4 + 4 + 16 + 4  # strip / bCooked / body / nav / guid / sockets
    lod_count = _i32(export_data, pos)
    pos += 4
    if lod_count < 1:
        raise StaticMeshError("没有 LOD")

    # LOD0
    pos += 2  # FStripDataFlags
    section_count = _i32(export_data, pos)
    pos += 4
    sections: list[MeshSection] = []
    for _ in range(section_count):
        mat_idx, first, tris, min_v, max_v = struct.unpack_from("<5i", export_data, pos)
        pos += 20 + 16  # 5 个 int + 4 个 bool(int32)
        sections.append(MeshSection(mat_idx, first, tris, min_v, max_v))
    pos += 4  # MaxDeviation

    # FPositionVertexBuffer（含 2 字节 strip flags）
    pos += 2
    stride, num_vertices = struct.unpack_from("<ii", export_data, pos)
    pos += 8
    elem_size, elem_count = struct.unpack_from("<ii", export_data, pos)
    pos += 8
    if stride != 12 or elem_size != 12 or elem_count != num_vertices:
        raise StaticMeshError(
            f"位置缓冲布局异常: stride={stride} elem={elem_size} count={elem_count} verts={num_vertices}")
    positions = np.frombuffer(export_data, dtype="<f4", count=num_vertices * 3, offset=pos)
    positions = positions.reshape(-1, 3).astype(np.float32)
    pos += elem_size * elem_count

    # FStaticMeshVertexBuffer：只需跳过切线/UV 数据
    pos += 2  # strip flags
    num_texcoords = _i32(export_data, pos)
    num_vb_vertices = _i32(export_data, pos + 4)
    pos += 8
    use_full_precision = _i32(export_data, pos)
    use_high_precision_tangent = _i32(export_data, pos + 4)
    pos += 8
    tangent_size, tangent_count = struct.unpack_from("<ii", export_data, pos)
    pos += 8 + tangent_size * tangent_count
    uv_size, uv_count = struct.unpack_from("<ii", export_data, pos)
    pos += 8 + uv_size * uv_count
    if num_vb_vertices != num_vertices:
        raise StaticMeshError(f"顶点数不一致: {num_vb_vertices} vs {num_vertices}")

    # FColorVertexBuffer
    pos += 2  # strip flags
    color_stride, color_count = struct.unpack_from("<ii", export_data, pos)
    pos += 8
    if color_count:
        color_size, color_elem = struct.unpack_from("<ii", export_data, pos)
        pos += 8 + color_size * color_elem

    # FRawStaticIndexBuffer：i32 标志 + i32 + 字节长度 + 数据
    index32 = _i32(export_data, pos)
    index_bytes = _i32(export_data, pos + 8)
    data_offset = pos + 12
    if index32 == 0:
        indices = np.frombuffer(export_data, dtype="<u2", count=index_bytes // 2, offset=data_offset)
    else:
        indices = np.frombuffer(export_data, dtype="<u4", count=index_bytes // 4, offset=data_offset)
    indices = indices.astype(np.uint32)

    return StaticMesh(positions=positions, indices=indices, sections=sections)
