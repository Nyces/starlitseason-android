"""uasset 包头解析（UE4.24 cooked 子集），用于资产普查与后续类型化解析。"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

UASSET_MAGIC = 0x9E2A83C1


class UAssetError(RuntimeError):
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
class PackageSummary:
    legacy_version: int
    ue4_version: int
    licensee_version: int
    custom_versions: list[tuple[bytes, int]]
    header_size: int
    package_name: str
    package_flags: int
    name_count: int
    name_offset: int
    export_count: int
    export_offset: int
    import_count: int
    import_offset: int
    names: list[str] = field(default_factory=list)


def parse_summary(data: bytes) -> PackageSummary:
    if len(data) < 64:
        raise UAssetError("数据过短")
    (magic,) = struct.unpack_from("<I", data, 0)
    if magic != UASSET_MAGIC:
        raise UAssetError(f"包魔数不匹配: 0x{magic:08X}")
    legacy_version, legacy_ue3, ue4_version, licensee = struct.unpack_from("<iiii", data, 4)
    pos = 20
    (custom_count,) = struct.unpack_from("<i", data, pos)
    pos += 4
    custom_versions = []
    for _ in range(custom_count):
        guid = data[pos:pos + 16]
        (value,) = struct.unpack_from("<i", data, pos + 16)
        pos += 20
        custom_versions.append((guid, value))
    (header_size,) = struct.unpack_from("<i", data, pos)
    pos += 4
    package_name, pos = _fstring(data, pos)
    (package_flags,) = struct.unpack_from("<I", data, pos)
    pos += 4
    name_count, name_offset, export_count, export_offset, import_count, import_offset = \
        struct.unpack_from("<iiiiii", data, pos)
    pos += 24
    summary = PackageSummary(
        legacy_version=legacy_version,
        ue4_version=ue4_version,
        licensee_version=licensee,
        custom_versions=custom_versions,
        header_size=header_size,
        package_name=package_name,
        package_flags=package_flags,
        name_count=name_count,
        name_offset=name_offset,
        export_count=export_count,
        export_offset=export_offset,
        import_count=import_count,
        import_offset=import_offset,
    )
    summary.names = parse_name_map(data, name_offset, name_count)
    return summary


def parse_name_map(data: bytes, offset: int, count: int) -> list[str]:
    names: list[str] = []
    pos = offset
    for _ in range(count):
        name, pos = _fstring(data, pos)
        pos += 4  # cooked 包内 FNameEntry 的 hash
        names.append(name)
    return names
