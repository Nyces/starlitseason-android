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
    bone_map: np.ndarray | None = None      # 分段内骨骼下标 -> ReferenceSkeleton 下标


@dataclass
class SkeletalMesh:
    positions: np.ndarray
    indices: np.ndarray
    sections: list[SkinSection] = field(default_factory=list)
    uvs: np.ndarray | None = None
    material_slots: list[str] = field(default_factory=list)
    bones: list[tuple[str, int]] = field(default_factory=list)
    # 绑定姿势：每骨骼的本地 FTransform（四元数 xyzw + 平移 + 缩放）
    bind_pose: np.ndarray | None = None
    # 蒙皮：每顶点 4 个 (骨骼索引, 权重)；骨骼索引是 LOD 骨骼表的下标
    influence_bones: np.ndarray | None = None
    influence_weights: np.ndarray | None = None
    bone_map: np.ndarray | None = None       # LOD 骨骼下标 -> ReferenceSkeleton 下标

    def bind_matrices(self) -> np.ndarray:
        """绑定姿势下每根骨骼的全局矩阵。"""
        if not self.bones:
            raise SkeletalMeshError("没有骨骼")
        locals_ = np.array([_transform_matrix(self.bind_pose[i]) for i in range(len(self.bones))],
                           dtype=np.float64)
        return _accumulate(self.bones, locals_)

    def posed_positions(self, rotations: dict[str, np.ndarray] | None = None) -> np.ndarray:
        """按骨骼旋转（父空间，绕骨骼原点）重新蒙皮，返回顶点位置。

        rotations 为空时返回绑定姿势的顶点（应与 positions 一致）。
        """
        if self.influence_bones is None or self.bind_pose is None:
            raise SkeletalMeshError("缺少蒙皮数据")
        index = {name: i for i, (name, _) in enumerate(self.bones)}
        locals_ = np.array([_transform_matrix(self.bind_pose[i]) for i in range(len(self.bones))],
                           dtype=np.float64)
        for name, rotation in (rotations or {}).items():
            bone = index.get(name)
            if bone is None:
                continue
            local = locals_[bone].copy()
            local[:3, :3] = np.asarray(rotation, dtype=np.float64) @ local[:3, :3]
            locals_[bone] = local

        globals_ = _accumulate(self.bones, locals_)
        bind = _accumulate(self.bones, np.array(
            [_transform_matrix(self.bind_pose[i]) for i in range(len(self.bones))],
            dtype=np.float64))
        skin = globals_ @ np.linalg.inv(bind)             # (B,4,4)

        used = np.zeros(len(self.bones), dtype=bool)
        used[self.influence_bones] = True
        if not used.all():                                # 未参与蒙皮的骨骼不必求逆
            skin[~used] = np.eye(4, dtype=np.float64)

        weights = self.influence_weights.astype(np.float64)      # (N,4)
        matrices = skin[self.influence_bones]                    # (N,4,4,4)
        rotated = np.einsum("nkij,nj->nki", matrices[..., :3, :3], self.positions)
        moved = rotated + matrices[..., :3, 3]
        return np.einsum("nk,nki->ni", weights, moved).astype(np.float32)


def _quat_matrix(q) -> np.ndarray:
    """UE 的 FQuat（x, y, z, w）转 3×3 旋转矩阵。"""
    x, y, z, w = (float(v) for v in q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]],
        dtype=np.float64)


def _transform_matrix(transform) -> np.ndarray:
    """FTransform（四元数 + 平移 + 缩放）转 4×4 矩阵。"""
    matrix = np.eye(4, dtype=np.float64)
    scale = np.asarray(transform[7:10], dtype=np.float64)
    matrix[:3, :3] = _quat_matrix(transform[:4]) * scale
    matrix[:3, 3] = np.asarray(transform[4:7], dtype=np.float64)
    return matrix


def _accumulate(bones: list[tuple[str, int]], locals_: np.ndarray) -> np.ndarray:
    """按父索引把本地矩阵累乘成全局矩阵。"""
    out = np.zeros_like(locals_)
    for i in range(len(bones)):
        parent = bones[i][1]
        out[i] = locals_[i] if parent < 0 or parent >= len(bones) else out[parent] @ locals_[i]
    return out


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
    pos += 4
    section_bones = np.frombuffer(data, dtype="<u2", count=bone_map, offset=pos).astype(np.int32)
    pos += bone_map * 2
    num_vertices = _i32(data, pos)
    pos += 4 + 4                                 # NumVertices + MaxBoneInfluences
    pos += 2 + 20                                # CorrespondClothAssetIndex + FClothingSectionData
    pos += 4 + _i32(data, pos) * 4               # DuplicatedVertices
    pos += 4 + _i32(data, pos) * 8               # DupVertIndexData
    pos += 4                                     # bDisabled
    return SkinSection(material_index, base_index, num_triangles, base_vertex, num_vertices,
                       section_bones), pos


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
    pos += 4
    poses = np.frombuffer(data, dtype="<f4", count=pose_count * 10, offset=pos)
    poses = poses.reshape(-1, 10).astype(np.float32)
    pos += pose_count * 40
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
    pos += 4
    bone_map = np.frombuffer(data, dtype="<u2", count=required, offset=pos).astype(np.int32)
    pos += required * 2

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
        # 每顶点 num_texcoords 套 UV，只取第 0 套
        uvs = raw.reshape(num_vertices, num_texcoords, 2)[:, 0, :].astype(np.float32)
    pos = uv_offset + uv_size * uv_count

    # 蒙皮权重：头部 5 个 int32（变量骨骼标志、stride、顶点数、元素大小、元素个数），
    # 数据是每顶点 4 个 uint8 骨骼下标 + 4 个 uint8 权重
    pos += 2                                     # 蒙皮权重 FStripDataFlags
    _, _, weight_vertices, weight_stride, weight_count = struct.unpack_from("<5i", data, pos)
    pos += 20
    if weight_stride != 8 or weight_count != num_vertices or weight_vertices != num_vertices:
        raise SkeletalMeshError(
            f"蒙皮权重缓冲异常 stride={weight_stride} count={weight_count}")
    raw = np.frombuffer(data, dtype=np.uint8, count=weight_count * weight_stride,
                        offset=pos).reshape(weight_count, weight_stride)
    lod_bones = raw[:, :4].astype(np.int32)
    influence_weights = raw[:, 4:].astype(np.float32) / 255.0
    # 权重里的骨骼下标是「分段内局部下标」，要按分段的 BoneMap 换成 ReferenceSkeleton 下标
    influence_bones = lod_bones
    for section in sections:
        if section.bone_map is None or section.bone_map.size == 0:
            continue
        lo = section.base_vertex_index
        hi = lo + section.num_vertices
        local = lod_bones[lo:hi]
        mapped = section.bone_map[np.clip(local, 0, section.bone_map.size - 1)]
        influence_bones = influence_bones.copy()
        influence_bones[lo:hi] = np.where(local < section.bone_map.size, mapped, local)

    if len(indices) == 0 or int(indices.max()) >= num_vertices:
        raise SkeletalMeshError("索引越界")
    return SkeletalMesh(positions=positions, indices=indices, sections=sections,
                        uvs=uvs, material_slots=slots, bones=bones,
                        bind_pose=poses, influence_bones=influence_bones,
                        influence_weights=influence_weights, bone_map=bone_map)
