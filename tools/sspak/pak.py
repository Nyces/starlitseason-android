"""UE4 pak v8 读取实现（仅覆盖本作实际使用到的特性）。

格式要点（已用零售包实测确认）：
- FPakInfo 位于文件尾部，魔数 0x5A6F12E1。
- 索引可被 AES-256-ECB 加密；索引内条目名以 FString 存储。
- 条目数据按 64KB 分块 Zlib 压缩，块偏移相对条目起点。
- 条目可选加密（flags bit0），加密块按 16 字节对齐后参与 AES。
"""

from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Iterable

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK_MAGIC = 0x5A6F12E1
PAK_MAGIC_LE = struct.pack("<I", PAK_MAGIC)
INFO_TAIL_SCAN = 2048

COMPRESSION_NONE = 0
COMPRESSION_ZLIB = 1

FLAG_ENCRYPTED = 0x01


class PakError(RuntimeError):
    pass


@dataclass
class PakEntry:
    name: str
    offset: int
    size: int
    usize: int
    method: int
    flags: int
    hash: bytes
    blocks: list[tuple[int, int]] = field(default_factory=list)
    block_size: int = 0

    @property
    def encrypted(self) -> bool:
        return bool(self.flags & FLAG_ENCRYPTED)


def _read_fstring(buf: bytes, pos: int) -> tuple[str, int]:
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


def _align(value: int, alignment: int = 16) -> int:
    return (value + alignment - 1) // alignment * alignment


class PakFile:
    """单个 pak 文件。索引在构造时一次性解析，条目数据按需读取。"""

    def __init__(self, path: str, key: bytes):
        self.path = path
        self.key = key
        self._fh = open(path, "rb")
        self.file_size = os.path.getsize(path)

        info = self._read_info()
        self.version = info["version"]
        self.index_offset = info["index_offset"]
        self.index_size = info["index_size"]
        self.encrypted_index = info["encrypted_index"]
        self.encryption_guid = info["guid"]
        self.compression_methods = info["compression_methods"]

        index = self._read(self.index_offset, self.index_size)
        if self.encrypted_index:
            index = self._decrypt(index)
        self.mount_point, pos = _read_fstring(index, 0)
        (count,) = struct.unpack_from("<i", index, pos)
        pos += 4

        self.entries: list[PakEntry] = []
        for _ in range(count):
            name, pos = _read_fstring(index, pos)
            offset, size, usize = struct.unpack_from("<qqq", index, pos)
            pos += 24
            (method,) = struct.unpack_from("<i", index, pos)
            pos += 4
            digest = index[pos:pos + 20]
            pos += 20
            blocks: list[tuple[int, int]] = []
            if method != COMPRESSION_NONE:
                (block_count,) = struct.unpack_from("<i", index, pos)
                pos += 4
                for _ in range(block_count):
                    start, end = struct.unpack_from("<qq", index, pos)
                    pos += 16
                    blocks.append((start, end))
            flags = index[pos]
            pos += 1
            (block_size,) = struct.unpack_from("<i", index, pos)
            pos += 4
            self.entries.append(
                PakEntry(name=name, offset=offset, size=size, usize=usize,
                         method=method, flags=flags, hash=digest,
                         blocks=blocks, block_size=block_size)
            )
        self.index_consumed = pos
        self.index_extra = len(index) - pos
        self._name_map = {e.name: e for e in self.entries}

    # ---- 读取基础 ----

    def _read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.file_size:
            raise PakError(f"越界读取 {self.path}: offset={offset} size={size}")
        self._fh.seek(offset)
        data = self._fh.read(size)
        if len(data) != size:
            raise PakError(f"短读 {self.path}: 期望 {size} 实际 {len(data)}")
        return data

    def _decrypt(self, data: bytes) -> bytes:
        aligned = len(data) // 16 * 16
        if aligned == 0:
            return data
        dec = Cipher(algorithms.AES(self.key), modes.ECB()).decryptor().update(data[:aligned])
        return dec + data[aligned:]

    def _read_info(self) -> dict:
        scan = min(INFO_TAIL_SCAN, self.file_size)
        tail = self._read(self.file_size - scan, scan)
        pos = tail.rfind(PAK_MAGIC_LE)
        if pos < 0:
            raise PakError(f"未找到 pak 魔数: {self.path}")
        info = tail[pos:]
        (version,) = struct.unpack_from("<i", info, 4)
        index_offset, index_size = struct.unpack_from("<QQ", info, 8)
        encrypted_index = info[44] != 0
        guid = info[45:61]
        methods = []
        if version >= 8:
            for i in range(5):
                raw = info[61 + i * 32:61 + (i + 1) * 32].split(b"\x00")[0]
                methods.append(raw.decode("latin1"))
        return {
            "version": version,
            "index_offset": index_offset,
            "index_size": index_size,
            "encrypted_index": encrypted_index,
            "guid": guid,
            "compression_methods": methods,
        }

    # ---- 条目访问 ----

    def get(self, name: str) -> PakEntry | None:
        return self._name_map.get(name)

    def iter_entries(self, suffix: str | None = None) -> Iterable[PakEntry]:
        if suffix is None:
            return iter(self.entries)
        return (e for e in self.entries if e.name.lower().endswith(suffix))

    def read_entry(self, entry: PakEntry, max_bytes: int | None = None) -> bytes:
        if entry.method == COMPRESSION_NONE:
            data = self._read(entry.offset + 53, entry.size)
            if entry.encrypted:
                data = self._decrypt(data)[:entry.size]
            return data[:max_bytes] if max_bytes else data

        if entry.method != COMPRESSION_ZLIB:
            raise PakError(f"暂不支持的压缩方式 {entry.method}: {entry.name}")

        out = bytearray()
        for start, end in entry.blocks:
            length = end - start
            if entry.encrypted:
                length = _align(length, 16)
            chunk = self._read(entry.offset + start, length)
            if entry.encrypted:
                chunk = self._decrypt(chunk)
            out += zlib.decompress(chunk)
            if max_bytes is not None and len(out) >= max_bytes:
                return bytes(out[:max_bytes])
        if max_bytes is not None:
            return bytes(out[:max_bytes])
        return bytes(out)

    def read_name(self, name: str, max_bytes: int | None = None) -> bytes:
        entry = self.get(name)
        if entry is None:
            raise PakError(f"条目不存在: {name}")
        return self.read_entry(entry, max_bytes)

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> "PakFile":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
