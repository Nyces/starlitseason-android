"""UE4.24 cooked 资产里的 FPropertyTag 属性流解析。

布局：FName Name, FName Type, int32 Size, int32 ArrayIndex, <类型附加头>,
uint8 HasGuid,[FGuid 16], Value[Size]，以 `None` 结束。

数组（ArrayProperty）的值以 int32 元素个数开头，其后是每个元素的标签；
结构体数组的每个元素同样是完整的标签，值内部再是一段以 None 结束的属性流。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .package import Package, _fstring


class PropertyError(RuntimeError):
    pass


@dataclass
class Property:
    name: str
    type: str
    size: int
    array_index: int
    offset: int                      # 值起点
    struct_name: str = ""
    inner_type: str = ""

    def value(self, data: bytes) -> bytes:
        return data[self.offset:self.offset + self.size]


def _fname(buf: bytes, pos: int) -> tuple[int, int]:
    return struct.unpack_from("<ii", buf, pos)


def parse_one(data: bytes, names: list[str], pos: int) -> tuple[Property | None, int]:
    """解析单个属性标签；遇到 None 返回 (None, pos+8)。"""
    idx, number = _fname(data, pos)
    name = names[idx] if 0 <= idx < len(names) else f"<{idx}>"
    if name == "None" and number == 0:
        return None, pos + 8
    type_idx, _ = _fname(data, pos + 8)
    ptype = names[type_idx] if 0 <= type_idx < len(names) else f"<{type_idx}>"
    size, array_index = struct.unpack_from("<ii", data, pos + 16)
    p = pos + 24
    struct_name = inner_type = ""
    if ptype == "StructProperty":
        si, _ = _fname(data, p)
        struct_name = names[si] if 0 <= si < len(names) else f"<{si}>"
        p += 8 + 16                  # StructName + StructGuid
    elif ptype == "BoolProperty":
        p += 1
    elif ptype in ("ByteProperty", "EnumProperty"):
        p += 8
    elif ptype in ("ArrayProperty", "SetProperty", "OptionalProperty"):
        ii, _ = _fname(data, p)
        inner_type = names[ii] if 0 <= ii < len(names) else f"<{ii}>"
        p += 8
    elif ptype == "MapProperty":
        p += 16
    if p >= len(data):
        raise PropertyError(f"属性 {name} 标签越界")
    if data[p]:
        p += 1 + 16
    else:
        p += 1
    if size < 0 or p + size > len(data):
        raise PropertyError(f"属性 {name} 值越界 ({p}+{size} > {len(data)})")
    return Property(name, ptype, size, array_index, p, struct_name, inner_type), p + size


def parse_properties(data: bytes, names: list[str], start: int = 0) -> tuple[list[Property], int]:
    props: list[Property] = []
    pos = start
    for _ in range(65536):
        prop, pos = parse_one(data, names, pos)
        if prop is None:
            return props, pos
        props.append(prop)
    raise PropertyError("属性流没有 None 终止符")


def parse_struct(data: bytes, names: list[str], prop: Property) -> list[Property]:
    """解析结构体属性内部（以 None 结束）的属性流。"""
    props, _ = parse_properties(data, names, prop.offset)
    return props


def array_items(data: bytes, names: list[str], prop: Property) -> list[list[Property]]:
    """解析结构体数组：每个元素返回其内部属性列表。

    零售包里元素的 Size 字段并不总是可信（会与后续元素重叠，偶尔还多出
    1 字节填充），因此以「元素内部属性流的 None 结尾」为边界逐个推进，
    并在当前位置附近做 0~3 字节的容错定位。
    """
    if prop.inner_type != "StructProperty":
        raise PropertyError(f"{prop.name} 不是结构体数组")
    items: list[list[Property]] = []
    stop = prop.offset + prop.size
    pos = prop.offset + 4
    while pos + 33 <= stop:
        found = None
        for delta in range(4):
            try:
                elem, _ = parse_one(data, names, pos + delta)
            except PropertyError:
                continue
            if elem is None or elem.type != "StructProperty":
                continue
            try:
                inner, inner_end = parse_properties(data, names, elem.offset)
            except PropertyError:
                continue
            if inner and inner_end <= stop:
                found = (inner, inner_end)
                break
        if found is None:
            break
        items.append(found[0])
        pos = found[1]
    return items


def resolve_index(pkg: Package, index: int) -> str:
    """把导入/导出索引解析成 /Game/... 路径（不含扩展名）。"""
    parts: list[str] = []
    cur = index
    for _ in range(24):
        if cur < 0:
            imp = pkg.imports[-cur - 1]
            parts.append(imp.object_name)
            cur = imp.outer_index
        elif cur > 0:
            exp = pkg.exports[cur - 1]
            parts.append(exp.name)
            cur = exp.outer_index
        else:
            break
    return "/".join(reversed(parts))


def object_index(data: bytes, prop: Property) -> int:
    return struct.unpack_from("<i", data, prop.offset)[0]


def name_text(data: bytes, prop: Property, names: list[str]) -> str:
    idx, _ = _fname(data, prop.offset)
    return names[idx] if 0 <= idx < len(names) else f"<{idx}>"


def fstring_value(data: bytes, prop: Property) -> str:
    text, _ = _fstring(data, prop.offset)
    return text
