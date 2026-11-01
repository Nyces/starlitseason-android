"""UE4.24 SkeletalMesh（cooked）：绑定姿势的 LOD0 几何 + 骨骼表。

只解出渲染绑定姿势所需的部分：材质槽、ReferenceSkeleton（骨骼名 + 父索引）、
LOD0 的顶点位置 / UV0 / 索引 / 分段。蒙皮权重暂时跳过（绑定姿势无需权重）。

cooked 布局（实测）：
    FStripDataFlags(2) Bounds(32) Materials[40] ReferenceSkeleton
    bCooked(4) LODCount(4) LOD*
    LOD: FStripDataFlags(2) bCookedOut(4) bInlined(4) RequiredBones
         分段 Sections / ActiveBones / [4] / 流式缓冲（索引、位置、UV、权重）
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .package import Package
from .properties import parse_properties, resolve_index

MATERIAL_SIZE = 40
BOUNDS_SIZE = 32
MESH_TO_MESH_VERT_DATA = 56


class SkeletalMeshError(RuntimeError):
    pass


@dataclass
class SkinSection:
    material_index: int
    base_index: int
    num_triangles: int
    base_vertex_index: int
    num_vertices: int


@dataclass
class SkeletalMesh:
    positions: np.ndarray
    indices: np.ndarray
    sections: list[SkinSection] = field(default_factory=list)
    uvs: np.ndarray | None = None
    material_slots: list[str] = field(default_factory=list)
    bones: list[tuple[str, int]] = field(default_factory=list)


def _i32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<i", data, pos)[0]


def _section(data: bytes, pos: int) -> tuple[SkinSection, int]:
    pos += 2                                     # FStripDataFlags
    (material_index,) = struct.unpack_from("<h", data, pos)
    pos += 2
    base_index = _i32(data, pos)
    num_triangles = _i32(data, pos + 4)
    pos += 8 + 4                                 # + bRecomputeTangent
    pos += 4                                     # bCastShadow
    (base_vertex,) = struct.unpack_from("<I", data, pos)
    pos += 4
    cloth_lods = _i32(data, pos)                 # TArray<TArray<FMeshToMeshVertData>>
    pos += 4
    for _ in range(cloth_lods):
        count = _i32(data, pos)
        pos += 4 + count * MESH_TO_MESH_VERT_DATA
    bone_map = _i32(data, pos)
    pos += 4 + bone_map * 2
    num_vertices = _i32(data, pos)
    pos += 4 + 4                                 # NumVertices + MaxBoneInfluences
    pos += 2 + 20                                # CorrespondClothAssetIndex + FClothingSectionData
    pos += 4 + _i32(data, pos) * 4               # DuplicatedVertices
    pos += 4 + _i32(data, pos) * 8               # DupVertIndexData
    pos += 4                                     # bDisabled
    return SkinSection(material_index, base_index, num_triangles, base_vertex, num_vertices), pos


def parse_skeletal_mesh(package: Package) -> SkeletalMesh:
    export = package.find_export("SkeletalMesh")
    if export is None:
        raise SkeletalMeshError("该资源不包含 SkeletalMesh")
    data = package.export_bytes(export)
    names = package.names
    props, pos = parse_properties(data, names)
    has_colors = any(p.name == "bHasVertexColors" and p.type == "BoolProperty" for p in props)

    pos += 2 + BOUNDS_SIZE
    material_count = _i32(data, pos)
    pos += 4
    slots: list[str] = []
    for _ in range(material_count):
        slots.append(resolve_index(package, _i32(data, pos)))
        pos += MATERIAL_SIZE

    bone_count = _i32(data, pos)
    pos += 4
    bones: list[tuple[str, int]] = []
    for _ in range(bone_count):
        name_index = _i32(data, pos)
        parent = _i32(data, pos + 8)
        name = names[name_index] if 0 <= name_index < len(names) else f"<{name_index}>"
        bones.append((name, parent))
        pos += 12
    pose_count = _i32(data, pos)
    pos += 4 + pose_count * 40                   # FTransform = 四元数 + 平移 + 缩放
    name_map = _i32(data, pos)
    pos += 4 + name_map * 12

    pos += 4                                     # bCooked
    lod_count = _i32(data, pos)
    pos += 4
    if lod_count < 1:
        raise SkeletalMeshError("没有 LOD")

    pos += 2                                     # LOD0 FStripDataFlags
    cooked_out = _i32(data, pos)
    inlined = _i32(data, pos + 4)
    pos += 8
    if cooked_out:
        raise SkeletalMeshError("LOD0 被剔除")
    required = _i32(data, pos)
    pos += 4 + required * 2

    section_count = _i32(data, pos)
    pos += 4
    sections: list[SkinSection] = []
    for _ in range(section_count):
        section, pos = _section(data, pos)
        sections.append(section)
    active = _i32(data, pos)
    pos += 4 + active * 2
    pos += 4                                     # 缓冲大小
    if not inlined:
        raise SkeletalMeshError("LOD0 数据在外部 bulk，暂不支持")

    pos += 2                                     # 流式数据 FStripDataFlags
    index_width = data[pos]
    pos += 1
    elem_size = _i32(data, pos)
    elem_count = _i32(data, pos + 4)
    pos += 8
    if index_width == 2:
        indices = np.frombuffer(data, dtype="<u2", count=elem_count, offset=pos)
    else:
        indices = np.frombuffer(data, dtype="<u4", count=elem_count, offset=pos)
    indices = indices.astype(np.uint32)
    pos += elem_size * elem_count

    stride = _i32(data, pos)
    num_vertices = _i32(data, pos + 4)
    pos += 8
    elem_size = _i32(data, pos)
    elem_count = _i32(data, pos + 4)
    pos += 8
    if stride != 12 or elem_size != 12 or elem_count != num_vertices:
        raise SkeletalMeshError(f"位置缓冲异常 stride={stride} elem={elem_size} count={elem_count}")
    positions = np.frombuffer(data, dtype="<f4", count=num_vertices * 3, offset=pos)
    positions = positions.reshape(-1, 3).astype(np.float32)
    pos += elem_size * elem_count

    pos += 2                                     # FStaticMeshVertexBuffer strip flags
    num_texcoords = _i32(data, pos)
    vb_vertices = _i32(data, pos + 4)
    pos += 8
    full_precision = _i32(data, pos)
    pos += 8
    tangent_size = _i32(data, pos)
    tangent_count = _i32(data, pos + 4)
    pos += 8 + tangent_size * tangent_count
    uv_size = _i32(data, pos)
    uv_count = _i32(data, pos + 4)
    uv_offset = pos + 8
    if vb_vertices != num_vertices:
        raise SkeletalMeshError(f"顶点数不一致 {vb_vertices} != {num_vertices}")

    uvs = None
    item = 8 if full_precision else 4
    if uv_size == item and uv_count == num_texcoords * num_vertices:
        raw = np.frombuffer(data, dtype="<f4" if full_precision else "<f2",
                            count=uv_count * 2, offset=uv_offset)
        uvs = raw.reshape(uv_count, 2)[::num_texcoords].astype(np.float32)

    if len(indices) == 0 or int(indices.max()) >= num_vertices:
        raise SkeletalMeshError("索引越界")
    return SkeletalMesh(positions=positions, indices=indices, sections=sections,
                        uvs=uvs, material_slots=slots, bones=bones)
