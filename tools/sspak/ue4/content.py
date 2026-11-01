"""按 /Game/... 路径从多个 pak 中定位并加载资产。"""

from __future__ import annotations

from dataclasses import dataclass

from .package import Package
from .texture import Texture2D, parse_texture

CONTENT_PREFIX = "content/"


def _key(name: str) -> str:
    lower = name.lower()
    index = lower.find(CONTENT_PREFIX)
    return lower[index + len(CONTENT_PREFIX):] if index >= 0 else lower


@dataclass
class ContentStore:
    paks: list

    def __post_init__(self) -> None:
        self._entries: dict[str, tuple] = {}
        for pak in self.paks:
            for entry in pak.entries:
                self._entries.setdefault(_key(entry.name), (pak, entry))

    @staticmethod
    def _game_path(asset: str) -> str:
        if asset.startswith("/Game/"):
            asset = asset[len("/Game/"):]
        elif asset.startswith("Game/"):
            asset = asset[len("Game/"):]
        parts = asset.split("/")
        # 对象路径 /A/B/B 与包路径 /A/B 等价
        if len(parts) >= 2 and parts[-1] == parts[-2]:
            parts = parts[:-1]
        return "/".join(parts)

    def _find(self, asset: str, ext: str):
        found = self._entries.get(_key(self._game_path(asset) + ext))
        if found is not None:
            return found
        return self._entries.get(_key(asset + ext))

    def has(self, asset: str) -> bool:
        return self._find(asset, ".uasset") is not None

    def package(self, asset: str) -> Package | None:
        uasset = self._find(asset, ".uasset") or self._find(asset, ".umap")
        if uasset is None:
            return None
        pak, entry = uasset
        data = pak.read_entry(entry)
        uexp = self._find(asset, ".uexp")
        return Package(data, pak.read_entry(uexp[1]) if uexp else b"")

    def texture(self, asset: str) -> Texture2D | None:
        """读取贴图；像素在 .ubulk 里，缺失或解析失败时返回 None。"""
        from .texture import TextureError

        pak_entry = self._find(asset, ".ubulk")
        package = self.package(asset)
        if package is None:
            return None
        ubulk = pak_entry[0].read_entry(pak_entry[1]) if pak_entry else b""
        try:
            return parse_texture(package, ubulk)
        except TextureError:
            return None
