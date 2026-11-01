"""UE4.24 StaticMesh：属性流 + LOD0 几何（位置/UV/索引/分段/材质槽）。

cooked 原生数据布局（实测零售包，位置均在导出数据内偏移）：
    [4] 未用                       FStripDataFlags(2) bCooked(4)
    BodySetup(4) NavCollision(4) LightingGuid(16) Sockets(4) LODCount(4)
    LOD0: FStripDataFlags(2) SectionCount(4) Sections[28] MaxDeviation(4)
          未用(8) FStripDataFlags(2) FPositionVertexBuffer
          FStaticMeshVertexBuffer FColorVertexBuffer FRawStaticIndexBuffer

FStaticMeshSection = 5×int32 + 2×bool(int32) = 28 字节。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .properties import array_items, object_index, parse_properties, resolve_index

SECTION_SIZE = 28
LOD_PREFIX = 8


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
    uvs: np.ndarray | None = None
    material_slots: list[str] = field(default_factory=list)


def _i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def _read_materials(data: bytes, package, props) -> list[str]:
    for prop in props:
        if prop.name == "StaticMaterials" and prop.inner_type == "StructProperty":
            slots: list[str] = []
            for items in array_items(data, package.names, prop):
                for item in items:
                    if item.name == "MaterialInterface" and item.type == "ObjectProperty":
                        slots.append(resolve_index(package, object_index(data, item)))
            return slots
    return []


def _parse_at(data: bytes, native: int) -> StaticMesh:
    pos = native
    if _i32(data, pos + 2) != 1:                       # bCooked
        raise StaticMeshError("bCooked 不为 1")
    pos += 2 + 4 + 4 + 4 + 16                          # strip/bCooked/body/nav/guid
    sockets = _i32(data, pos)
    pos += 4
    lod_count = _i32(data, pos)
    pos += 4
    if sockets != 0 or not 1 <= lod_count <= 8:
        raise StaticMeshError(f"sockets={sockets} lod={lod_count}")

    pos += 2
    section_count = _i32(data, pos)
    pos += 4
    if not 1 <= section_count <= 128:
        raise StaticMeshError(f"section_count={section_count}")
    sections: list[MeshSection] = []
    for _ in range(section_count):
        sections.append(MeshSection(*struct.unpack_from("<5i", data, pos)))
        pos += SECTION_SIZE
    pos += 4 + LOD_PREFIX

    pos += 2
    stride, num_vertices = struct.unpack_from("<ii", data, pos)
    pos += 8
    elem_size, elem_count = struct.unpack_from("<ii", data, pos)
    pos += 8
    if stride != 12 or elem_size != 12 or elem_count != num_vertices or not 0 < num_vertices < 4_000_000:
        raise StaticMeshError(f"位置缓冲异常 stride={stride} elem={elem_size} "
                              f"count={elem_count} verts={num_vertices}")
    positions = np.frombuffer(data, dtype="<f4", count=num_vertices * 3, offset=pos)
    positions = positions.reshape(-1, 3).astype(np.float32)
    pos += elem_size * elem_count

    pos += 2
    num_texcoords = _i32(data, pos)
    num_vb_vertices = _i32(data, pos + 4)
    pos += 8
    use_full_precision = _i32(data, pos)
    pos += 8
    tangent_size, tangent_count = struct.unpack_from("<ii", data, pos)
    pos += 8 + tangent_size * tangent_count
    uv_size, uv_count = struct.unpack_from("<ii", data, pos)
    uv_offset = pos + 8
    pos += 8 + uv_size * uv_count
    if num_vb_vertices != num_vertices or not 1 <= num_texcoords <= 8:
        raise StaticMeshError(f"顶点缓冲异常 texcoords={num_texcoords} verts={num_vb_vertices}")

    item = 8 if use_full_precision else 4
    uvs = None
    if uv_size == item and uv_count == num_texcoords * num_vertices:
        raw = np.frombuffer(data, dtype="<f4" if use_full_precision else "<f2",
                            count=uv_count * 2, offset=uv_offset)
        # 每顶点 num_texcoords 套 UV，只取第 0 套
        uvs = raw.reshape(num_vertices, num_texcoords, 2)[:, 0, :].astype(np.float32)

    pos += 2
    color_stride, color_count = struct.unpack_from("<ii", data, pos)
    pos += 8
    if color_count:
        color_size, color_elem = struct.unpack_from("<ii", data, pos)
        pos += 8 + color_size * color_elem

    index32 = _i32(data, pos)
    index_bytes = _i32(data, pos + 8)
    data_offset = pos + 12
    if index32 == 0:
        indices = np.frombuffer(data, dtype="<u2", count=index_bytes // 2, offset=data_offset)
    else:
        indices = np.frombuffer(data, dtype="<u4", count=index_bytes // 4, offset=data_offset)
    indices = indices.astype(np.uint32)
    if len(indices) == 0 or int(indices.max()) >= num_vertices:
        raise StaticMeshError("索引越界")
    for section in sections:
        if section.first_index + section.num_triangles * 3 > len(indices):
            raise StaticMeshError("分段索引越界")

    return StaticMesh(positions=positions, indices=indices, sections=sections, uvs=uvs)


def parse_static_mesh(export_data: bytes, package) -> StaticMesh:
    props, props_end = parse_properties(export_data, package.names)
    last_error: Exception | None = None
    for native in range(props_end, min(props_end + 16, len(export_data) - 64)):
        try:
            mesh = _parse_at(export_data, native)
        except (StaticMeshError, struct.error) as exc:
            last_error = exc
            continue
        mesh.material_slots = _read_materials(export_data, package, props)
        return mesh
    raise StaticMeshError(f"未找到可解析的 StaticMesh 原生数据: {last_error}")
