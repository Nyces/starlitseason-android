"""UE4.24 cooked 包解析：摘要、名称表、导入/导出表、导出数据定位。

零售包是 "unversioned" 的：包头里的版本字段为 0，实际按 4.24 布局解析。
导出表的 SerialOffset 相对 (uasset 包头 + uexp) 的拼接起点，
因此 uexp 内偏移 = SerialOffset - TotalHeaderSize。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

UASSET_MAGIC = 0x9E2A83C1


class PackageError(RuntimeError):
    pass


def _fstring(buf: bytes, pos: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<i", buf, pos)
    pos += 4
    if length == 0:
        return "", pos
    if length < 0:
        count = -length
        text = buf[pos:pos + count * 2].decode("utf-16-le", "replace")
        return text.rstrip("\x00"), pos + count * 2
    text = buf[pos:pos + length].decode("latin1", "replace")
    return text.rstrip("\x00"), pos + length


@dataclass
class ImportObject:
    class_package: str
    class_name: str
    outer_index: int
    object_name: str


@dataclass
class ExportObject:
    name: str
    class_index: int
    outer_index: int
    serial_size: int
    serial_offset: int
    class_name: str = ""


@dataclass
class Package:
    uasset: bytes
    uexp: bytes = b""
    package_name: str = ""
    header_size: int = 0
    names: list[str] = field(default_factory=list)
    imports: list[ImportObject] = field(default_factory=list)
    exports: list[ExportObject] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._parse()

    def _parse(self) -> None:
        data = self.uasset
        if len(data) < 64:
            raise PackageError("uasset 过短")
        (magic,) = struct.unpack_from("<I", data, 0)
        if magic != UASSET_MAGIC:
            raise PackageError(f"包魔数不匹配: 0x{magic:08X}")

        pos = 8
        (legacy,) = struct.unpack_from("<i", data, 4)
        if legacy != -4:
            pos += 4
        pos += 8  # FileVersionUE4 / Licensee（零售包为 0）
        (custom_count,) = struct.unpack_from("<i", data, pos)
        pos += 4 + custom_count * 20

        (self.header_size,) = struct.unpack_from("<i", data, pos)
        pos += 4
        self.package_name, pos = _fstring(data, pos)
        pos += 4  # PackageFlags
        name_count, name_offset, _soft_n, _soft_o, export_count, export_offset, \
            import_count, import_offset = struct.unpack_from("<iiiiiiii", data, pos)
        pos += 32
        (depends_offset,) = struct.unpack_from("<i", data, pos)

        q = name_offset
        for _ in range(name_count):
            name, q = _fstring(data, q)
            q += 4  # cooked 名称项哈希
            self.names.append(name)

        p = import_offset
        for _ in range(import_count):
            cp_i, _cp_n, cn_i, _cn_n = struct.unpack_from("<iiii", data, p)
            (outer,) = struct.unpack_from("<i", data, p + 16)
            on_i, _on_n = struct.unpack_from("<ii", data, p + 20)
            p += 28
            self.imports.append(ImportObject(
                class_package=self._name(cp_i),
                class_name=self._name(cn_i),
                outer_index=outer,
                object_name=self._name(on_i),
            ))

        stride = (depends_offset - export_offset) // export_count if export_count else 0
        p = export_offset
        for _ in range(export_count):
            class_index, _super, _template, outer = struct.unpack_from("<iiii", data, p)
            ni, _nn = struct.unpack_from("<ii", data, p + 16)
            serial_size, serial_offset = struct.unpack_from("<qq", data, p + 28)
            export = ExportObject(
                name=self._name(ni),
                class_index=class_index,
                outer_index=outer,
                serial_size=serial_size,
                serial_offset=serial_offset,
            )
            export.class_name = self._class_name(class_index)
            self.exports.append(export)
            p += stride

    def _name(self, index: int) -> str:
        if 0 <= index < len(self.names):
            return self.names[index]
        return f"<name {index}>"

    def _class_name(self, class_index: int) -> str:
        if class_index < 0:
            idx = -class_index - 1
            if 0 <= idx < len(self.imports):
                return self.imports[idx].object_name
            return f"<import {idx}>"
        if 0 <= class_index < len(self.exports):
            return self.exports[class_index].name
        return "<null>"

    def export_bytes(self, export: ExportObject) -> bytes:
        offset = export.serial_offset - self.header_size
        if offset < 0 or offset + export.serial_size > len(self.uexp):
            raise PackageError(f"导出越界: {export.name}")
        return self.uexp[offset:offset + export.serial_size]

    def find_export(self, class_name: str) -> ExportObject | None:
        return next((e for e in self.exports if e.class_name == class_name), None)
