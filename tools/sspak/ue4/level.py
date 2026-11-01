"""关卡：从 cooked 地图包中取出摆放在关卡里的静态网格 actor。

cooked 地图不再保留 ULevel.Actors 属性，actor 是以 Level 为 outer 的导出；
组件变换通常为默认值（舞台网格本身按世界坐标建模），若存在
RelativeLocation / RelativeRotation / RelativeScale3D 则一并读出。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .package import Package
from .properties import object_index, parse_properties, resolve_index

ACTOR_TRANSFORM_PROPS = ("RelativeLocation", "RelativeRotation", "RelativeScale3D")


@dataclass
class PlacedMesh:
    mesh: str
    location: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: tuple[float, float, float] = (0.0, 0.0, 0.0)   # pitch, yaw, roll
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)
    materials: list[str] = field(default_factory=list)
    actor_class: str = ""


def _struct_floats(data: bytes, prop) -> tuple[float, ...]:
    count = prop.size // 4
    return struct.unpack_from(f"<{count}f", data, prop.offset)


def parse_level(package: Package) -> list[PlacedMesh]:
    level = next((e for e in package.exports if e.class_name == "Level"), None)
    if level is None:
        raise ValueError("地图包中没有 Level")
    level_index = package.exports.index(level) + 1

    placed: list[PlacedMesh] = []
    for actor in package.exports:
        if actor.outer_index != level_index or actor.class_name == "Level":
            continue
        try:
            data = package.export_bytes(actor)
            props, _ = parse_properties(data, package.names)
        except Exception:  # noqa: BLE001 - 非 actor 导出直接跳过
            continue
        component_index = 0
        for prop in props:
            if prop.type == "ObjectProperty" and prop.name in ("StaticMeshComponent", "RootComponent"):
                component_index = object_index(data, prop)
                if prop.name == "StaticMeshComponent":
                    break
        if component_index <= 0:
            continue
        component = package.exports[component_index - 1]
        comp_data = package.export_bytes(component)
        try:
            comp_props, _ = parse_properties(comp_data, package.names)
        except Exception:  # noqa: BLE001
            continue

        mesh = None
        transform: dict[str, tuple[float, ...]] = {}
        materials: list[str] = []
        for prop in comp_props:
            if prop.name == "StaticMesh" and prop.type == "ObjectProperty":
                mesh = resolve_index(package, object_index(comp_data, prop))
            elif prop.type == "StructProperty" and prop.name in ACTOR_TRANSFORM_PROPS:
                transform[prop.name] = _struct_floats(comp_data, prop)
            elif prop.name == "OverrideMaterials" and prop.inner_type == "ObjectProperty":
                count = struct.unpack_from("<i", comp_data, prop.offset)[0]
                base = prop.offset + 4
                for index in range(count):
                    (obj,) = struct.unpack_from("<i", comp_data, base + index * 4)
                    path = resolve_index(package, obj)
                    materials.append(path if obj != 0 else "")
        if mesh is None:
            continue
        placed.append(PlacedMesh(
            mesh=mesh,
            location=transform.get("RelativeLocation", (0.0, 0.0, 0.0)),
            rotation=transform.get("RelativeRotation", (0.0, 0.0, 0.0)),
            scale=transform.get("RelativeScale3D", (1.0, 1.0, 1.0)),
            materials=materials,
            actor_class=actor.class_name,
        ))
    return placed
