"""材质实例：从 MaterialInstanceConstant 里取贴图/标量参数与父材质。

cooked 资产保留了 TextureParameterValues / ScalarParameterValues，
参数值直接指向贴图包（/Game/...），父材质链需要逐级回溯。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .package import Package
from .properties import (PropertyError, array_items, object_index, parse_properties,
                         parse_struct, resolve_index)


@dataclass
class MaterialInstance:
    textures: dict[str, str] = field(default_factory=dict)
    scalars: dict[str, float] = field(default_factory=dict)
    vectors: dict[str, tuple[float, float, float, float]] = field(default_factory=dict)
    parent: str | None = None


def _param_name(items, data, names) -> str | None:
    from .properties import name_text

    for item in items:
        if item.name == "ParameterInfo" and item.type == "StructProperty":
            for sub in parse_struct(data, names, item):
                if sub.name == "Name":
                    return name_text(data, sub, names)
    return None


def parse_material_instance(package: Package) -> MaterialInstance:
    result = MaterialInstance()
    for export in package.exports:
        if export.class_name not in ("MaterialInstanceConstant", "MaterialInstance"):
            continue
        data = package.export_bytes(export)
        props, _ = parse_properties(data, package.names)
        for prop in props:
            if prop.name == "Parent" and prop.type == "ObjectProperty":
                result.parent = resolve_index(package, object_index(data, prop))
            elif prop.name.endswith("ParameterValues") and prop.inner_type == "StructProperty":
                for items in array_items(data, package.names, prop):
                    name = _param_name(items, data, package.names)
                    if not name:
                        continue
                    for item in items:
                        if item.name != "ParameterValue":
                            continue
                        if item.type == "ObjectProperty":
                            result.textures[name] = resolve_index(package, object_index(data, item))
                        elif item.type == "FloatProperty":
                            result.scalars[name] = struct.unpack_from("<f", data, item.offset)[0]
                        elif item.type == "StructProperty" and item.struct_name == "LinearColor":
                            result.vectors[name] = struct.unpack_from("<4f", data, item.offset)
        return result
    raise PropertyError("该资源不是材质实例")


def pick_base_color(textures: dict[str, str]) -> tuple[str, str] | None:
    """从贴图参数里挑一张最像"基础色"的贴图。"""
    if not textures:
        return None
    strong = ("basetexture", "basecolor", "base_color", "albedo", "diffuse", "colortex")
    weak = ("color", "tex", "txt")
    negative = ("mask", "lut", "sdw", "shadow", "nml", "normal", "nrm", "spec",
                "rough", "metal", "opacity", "ref", "outline", "sss")

    def score(name: str) -> int:
        low = name.lower()
        if any(word in low for word in negative):
            return -20
        if any(word in low for word in strong):
            return 10
        return sum(word in low for word in weak)

    name, path = max(textures.items(), key=lambda kv: (score(kv[0]), -len(kv[0])))
    return (name, path) if score(name) > 0 else None
