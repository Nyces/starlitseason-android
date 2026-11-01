"""UTexture2D（cooked PC）：平台数据头 + mip0 像素。

cooked 纹理的 mip 表里只记录尺寸与 .ubulk 偏移，像素本体在 <资产>.ubulk：
首个 mip 位于 ubulk 起始处，其后依次为更小的 mip。静态预览只需要 mip0。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

from .package import Package
from .properties import parse_properties


class TextureError(RuntimeError):
    pass


# 像素格式 → (每 4x4 块或每像素字节数, 解码器)
PIXEL_FORMATS: dict[str, tuple[int, str]] = {
    "PF_DXT1": (8, "bc1"),
    "PF_DXT5": (16, "bc3"),
    "PF_BC4": (8, "bc4"),
    "PF_BC5": (16, "bc5"),
    "PF_BC7": (16, "bc7"),
    "PF_B8G8R8A8": (4, "bgra8"),
    "PF_R8G8B8A8": (4, "rgba8"),
    "PF_G8": (1, "g8"),
    "PF_A8": (1, "g8"),
    "PF_R16F": (2, "r16f"),
}


@dataclass
class Texture2D:
    width: int
    height: int
    pixel_format: str
    mip0: bytes

    @property
    def mip0_size(self) -> int:
        return mip0_size(self.pixel_format, self.width, self.height)


def mip0_size(pixel_format: str, width: int, height: int) -> int:
    spec = PIXEL_FORMATS.get(pixel_format)
    if spec is None:
        raise TextureError(f"暂不支持的像素格式 {pixel_format}")
    unit, _ = spec
    if pixel_format.startswith(("PF_DXT", "PF_BC")):
        return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * unit
    return width * height * unit


def mip_chain_sizes(pixel_format: str, width: int, height: int, mips: int) -> list[int]:
    sizes: list[int] = []
    for _ in range(max(mips, 1)):
        sizes.append(mip0_size(pixel_format, max(width, 1), max(height, 1)))
        width, height = width // 2, height // 2
    return sizes


def _locate_platform_data(data: bytes, start: int) -> tuple[int, str]:
    """定位平台数据头：返回 (PixelFormat 字符串起点对应的长度字段位置, 格式名)。

    不同资产的属性流之后可能多出若干字节，因此以 "PF_" 字符串为锚点反推。
    """
    for pos in range(start, min(start + 128, len(data) - 8)):
        (length,) = struct.unpack_from("<i", data, pos)
        if not 3 <= length <= 24:
            continue
        text = data[pos + 4:pos + 4 + length]
        if len(text) != length or not text.endswith(b"\x00"):
            continue
        name = text[:-1].decode("latin1", "replace")
        if name.startswith("PF_") and name in PIXEL_FORMATS:
            return pos, name
    raise TextureError("未找到纹理平台数据（PixelFormat）")


def parse_texture(package: Package, ubulk: bytes) -> Texture2D:
    export = next((e for e in package.exports if e.class_name in ("Texture2D", "Texture2DArray")), None)
    if export is None:
        raise TextureError("该资源不包含 Texture2D")
    data = package.export_bytes(export)
    _props, props_end = parse_properties(data, package.names)

    length_pos, pixel_format = _locate_platform_data(data, props_end)
    width, height, _packed = struct.unpack_from("<iiI", data, length_pos - 12)
    if width <= 0 or height <= 0 or width > 16384 or height > 16384:
        raise TextureError(f"纹理尺寸异常 {width}x{height}")

    need = mip0_size(pixel_format, width, height)
    if len(ubulk) >= need:
        return Texture2D(width, height, pixel_format, ubulk[:need])

    # 像素内联在导出数据末尾：按 mip 链总长度回推 mip0 起点
    header_end = length_pos + 4 + struct.unpack_from("<i", data, length_pos)[0]
    mips = struct.unpack_from("<ii", data, header_end)[1] if header_end + 8 <= len(data) else 1
    total = sum(mip_chain_sizes(pixel_format, width, height, max(mips, 1)))
    start = len(data) - total
    if start < 0 or start + need > len(data):
        raise TextureError(f"ubulk 不足且内联数据缺失：需要 {need}，实际 {len(ubulk)}")
    return Texture2D(width, height, pixel_format, data[start:start + need])


def decode_texture(texture: Texture2D) -> np.ndarray:
    """解码 mip0，返回 (H, W, 4) uint8，行序自上而下。"""
    kind = PIXEL_FORMATS[texture.pixel_format][1]
    width, height = texture.width, texture.height
    data = texture.mip0

    if kind in ("bc1", "bc3", "bc4", "bc5", "bc7"):
        import texture2ddecoder

        fn = {
            "bc1": texture2ddecoder.decode_bc1,
            "bc3": texture2ddecoder.decode_bc3,
            "bc4": texture2ddecoder.decode_bc4,
            "bc5": texture2ddecoder.decode_bc5,
            "bc7": texture2ddecoder.decode_bc7,
        }[kind]
        rgba = fn(data, width, height)
        # texture2ddecoder 返回 BGRA，需要换回 RGBA
        return np.frombuffer(rgba, dtype=np.uint8).reshape(height, width, 4)[:, :, [2, 1, 0, 3]].copy()

    raw = np.frombuffer(data, dtype=np.uint8)
    if kind == "bgra8":
        return raw.reshape(height, width, 4)[:, :, [2, 1, 0, 3]].copy()
    if kind == "rgba8":
        return raw.reshape(height, width, 4).copy()
    if kind == "g8":
        gray = raw.reshape(height, width)
        return np.stack([gray, gray, gray, np.full_like(gray, 255)], axis=-1)
    raise TextureError(f"暂不支持解码 {texture.pixel_format}")
