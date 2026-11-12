"""sspak 命令行：pak 校验、资产普查、条目列举与提取。"""

from __future__ import annotations

import argparse
import collections
import os
import sys
from dataclasses import dataclass

from .pak import PakFile, PakError, COMPRESSION_ZLIB, COMPRESSION_NONE
from .uasset import UASSET_MAGIC, UAssetError, parse_summary


def _key(text: str | None) -> bytes:
    raw = text or os.environ.get("SSPAK_KEY")
    if not raw:
        raise SystemExit("需要 --key 或环境变量 SSPAK_KEY（64 位十六进制）")
    raw = raw.strip().lower()
    if raw.startswith("0x"):
        raw = raw[2:]
    if len(raw) != 64:
        raise SystemExit("AES 密钥必须为 64 位十六进制")
    try:
        return bytes.fromhex(raw)
    except ValueError:
        raise SystemExit("AES 密钥包含非法字符")


def _paks(args) -> list[str]:
    paths = list(args.pak or [])
    if args.paks:
        for name in sorted(os.listdir(args.paks)):
            if not name.lower().endswith(".pak"):
                continue
            if not args.include_mods and name.lower().startswith(("add", "free", "lyric", "song")):
                continue
            if not args.include_mods and os.path.isdir(os.path.join(args.paks, "~mods")):
                pass
            paths.append(os.path.join(args.paks, name))
    if not paths:
        raise SystemExit("未找到 pak 文件")
    return paths


def _open_all(args) -> list[PakFile]:
    key = _key(args.key)
    return [PakFile(p, key) for p in _paks(args)]


def cmd_verify(args) -> int:
    paks = _open_all(args)
    sampled = 0
    failed = 0
    try:
        for pak in paks:
            print(f"\n== {os.path.basename(pak.path)}")
            print(f"   version={pak.version} entries={len(pak.entries):,} "
                  f"index_enc={pak.encrypted_index} index_extra={pak.index_extra} "
                  f"methods={pak.compression_methods}")
            print(f"   mount={pak.mount_point!r}")

            picks = [e for e in pak.entries
                     if e.name.lower().endswith((".uasset", ".uexp", ".png", ".mp4", ".awb", ".ini"))]
            step = max(1, len(picks) // max(1, args.sample))
            for entry in picks[::step][:args.sample]:
                try:
                    if args.full:
                        payload = pak.read_entry(entry)
                    else:
                        payload = pak.read_entry(entry, max_bytes=min(65536, entry.usize))
                    if not args.full and len(payload) < min(65536, entry.usize):
                        raise PakError("前缀读取不足")
                    if not args.full and len(payload) != min(65536, entry.usize):
                        pass
                    if args.full and len(payload) != entry.usize:
                        raise PakError(f"长度不符 期望 {entry.usize} 实际 {len(payload)}")
                    if entry.name.lower().endswith(".uasset"):
                        summary = parse_summary(payload)
                        if not summary.package_name:
                            raise UAssetError("包名为空")
                except Exception as exc:  # noqa: BLE001 - 校验阶段需要汇总所有错误
                    failed += 1
                    print(f"   [FAIL] {entry.name}: {exc}")
                sampled += 1
            print(f"   抽样 {sampled} 条，失败 {failed} 条")
    finally:
        for pak in paks:
            pak.close()
    print(f"\n合计抽样 {sampled} 条，失败 {failed} 条")
    return 0 if failed == 0 else 1


def cmd_list(args) -> int:
    paks = _open_all(args)
    try:
        shown = 0
        for pak in paks:
            for entry in pak.entries:
                if args.filter and args.filter.lower() not in entry.name.lower():
                    continue
                print(f"{os.path.basename(pak.path)}\t{entry.name}")
                shown += 1
                if args.limit and shown >= args.limit:
                    return 0
    finally:
        for pak in paks:
            pak.close()
    return 0


def cmd_extract(args) -> int:
    paks = _open_all(args)
    try:
        for pak in paks:
            entry = pak.get(args.name) or next(
                (e for e in pak.entries if e.name.lower() == args.name.lower()), None)
            if entry is None:
                continue
            payload = pak.read_entry(entry)
            os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
            with open(args.out, "wb") as fh:
                fh.write(payload)
            print(f"已提取 {entry.name} -> {args.out} ({len(payload):,} bytes)")
            return 0
    finally:
        for pak in paks:
            pak.close()
    print(f"未找到条目: {args.name}", file=sys.stderr)
    return 1


def cmd_census(args) -> int:
    paks = _open_all(args)
    ext = collections.Counter()
    top = collections.Counter()
    kinds = collections.Counter()
    names: dict[str, str] = {}
    errors: list[str] = []
    header_ok = 0
    header_bad = 0
    try:
        for pak in paks:
            for entry in pak.entries:
                names.setdefault(entry.name, os.path.basename(pak.path))
                lower = entry.name.lower()
                if "." in lower:
                    ext[lower.rsplit(".", 1)[1]] += 1
                parts = [p for p in entry.name.split("/") if p]
                if parts:
                    top["/".join(parts[:2]) if len(parts) > 1 else parts[0]] += 1
                if lower.endswith(".uasset"):
                    kinds["uasset"] += 1
                elif lower.endswith(".uexp"):
                    kinds["uexp"] += 1
                elif lower.endswith(".ubulk"):
                    kinds["ubulk"] += 1
                elif lower.endswith(".umap"):
                    kinds["umap"] += 1
        print(f"条目总数 {sum(len(p.entries) for p in paks):,}，唯一路径 {len(names):,}")
        print("扩展名：", dict(ext.most_common(20)))
        print("顶层目录：", dict(top.most_common(20)))

        if args.headers:
            for pak in paks:
                for entry in pak.entries:
                    if not entry.name.lower().endswith(".uasset"):
                        continue
                    last_error = None
                    for limit in (65536, 262144, 1048576):
                        try:
                            data = pak.read_entry(entry, max_bytes=limit)
                            summary = parse_summary(data)
                            if summary.package_name == "" and summary.name_count != 0:
                                raise UAssetError("包头异常")
                            header_ok += 1
                            last_error = None
                            break
                        except Exception as exc:  # noqa: BLE001
                            last_error = exc
                    if last_error is not None:
                        header_bad += 1
                        if len(errors) < 40:
                            errors.append(
                                f"{os.path.basename(pak.path)}:{entry.name}: {last_error}")
            print(f"\nuasset 包头解析：成功 {header_ok:,}，失败 {header_bad:,}")
            for line in errors:
                print("  ", line)
    finally:
        for pak in paks:
            pak.close()
    return 0 if (not args.headers or header_bad == 0) else 1


def _material_texture(store, material: str | None):
    """把材质（含父链）解析成基础色贴图，返回 (参数名, 贴图数组)。"""
    from .ue4.material import parse_material_instance, pick_base_color
    from .ue4.texture import decode_texture

    for _ in range(6):
        if not material:
            return None, None
        package = store.package(material)
        if package is None:
            return None, None
        try:
            instance = parse_material_instance(package)
        except Exception:  # noqa: BLE001 - 父级通常是基础材质（无参数可读）
            return None, None
        picked = pick_base_color(instance.textures)
        if picked:
            param, texture_path = picked
            texture = store.texture(texture_path)
            if texture is None:
                return param, None
            return param, decode_texture(texture)
        material = instance.parent
    return None, None


@dataclass
class ToonMaterial:
    """由材质实例反解出的渲染参数（默认值对应 MPC ToonShaderParameter 的真值）。"""

    texture: object = None
    shadow: tuple[float, float, float] = (1.0, 1.0, 1.0)
    threshold: float = 0.47
    blend: tuple[float, float, float] = (1.0, 1.0, 1.0)
    add: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rim_scale: float = 7.5
    sphere: object = None
    sphere_blend: tuple[float, float, float] = (1.0, 1.0, 1.0)
    sphere_pow: float = 1.0
    sphere_boost: float = 1.0
    shadow_map: object = None
    emissive: object = None
    emissive_boost: float = 1.0
    blend_mode: str = "opaque"
    unlit: bool = False
    screen: bool = False
    outline: bool = False
    outline_color: tuple[float, float, float] = (0.16, 0.10, 0.09)
    outline_width: float = 0.0008
    skip: bool = False                   # 腮红这类纯叠色层，当前不画

    def item(self, positions, indices, uvs=None, model=None):
        from .render import SceneItem

        return SceneItem(positions, indices, uvs, self.texture, model,
                         shadow_tint=self.shadow, threshold=self.threshold,
                         blend=self.blend, add=self.add, sphere=self.sphere,
                         sphere_blend=self.sphere_blend, sphere_pow=self.sphere_pow,
                         sphere_boost=self.sphere_boost, rim_scale=self.rim_scale,
                         shadow_map=self.shadow_map,
                         emissive=self.emissive, emissive_boost=self.emissive_boost,
                         blend_mode=self.blend_mode, unlit=self.unlit, screen=self.screen,
                         outline=self.outline, outline_color=self.outline_color,
                         outline_width=self.outline_width)


def _toon_params(store, material: str | None):
    """按 Toon 规则解析材质，返回 ToonMaterial。

    阴影色来自材质的 CelShadowTable / ToonShadowTable（2×256 的两色斜坡，
    暗端是粉/米色，亮端是白）；头发用 0.94 的阶跃阈值，其余 0.47。
    皮肤等材质没有基础色贴图，就用色表暗端作为底色。
    SphereMap / RimScale / OutlineScale 等按材质实例里的真实值取。
    """
    import numpy as np

    from .ue4.material import (parse_material_instance, parse_material_settings,
                               pick_base_color)
    from .ue4.texture import decode_texture

    result = ToonMaterial()
    is_outline = bool(material and "outline" in material.lower())
    # 眼睛相关材质（eyebase / eyelens / iris）的 SphereMap 是一张反光贴图，按视线空间
    # 法线取样；左右眼法线互为镜像，采样到的位置不对称，结果两眼反光不同（虹膜材质名是
    # face_iris，不含 eye，必须单独算进来）。
    is_eye = bool(material and any(tag in material.lower() for tag in ("eye", "iris")))

    # 先把父链走完：混合模式在基础材质上，不能因为拿到贴图就提前返回
    chain = []
    settings = None
    current = material
    for _ in range(6):
        if not current:
            break
        package = store.package(current)
        if package is None:
            break
        try:
            instance = parse_material_instance(package)
        except Exception:  # noqa: BLE001 - 父级是基础材质
            settings = parse_material_settings(package)
            if "framebuffer" in current.lower():
                result.unlit = True
                result.texture = np.full((1, 1, 4), (46, 50, 58, 255), dtype=np.uint8)
            break
        chain.append(instance)
        current = instance.parent

    if settings is not None:
        mode = settings.blend_mode.lower()
        if "additive" in mode:
            result.blend_mode = "additive"
        elif "modulate" in mode:
            result.blend_mode = "modulate"
        elif "translucent" in mode or "alphacomposite" in mode:
            result.blend_mode = "translucent"
        elif "masked" in mode:
            result.blend_mode = "masked"
        if "unlit" in settings.shading_model.lower():
            result.unlit = True
    if result.texture is not None:          # 屏幕抓帧材质的占位色
        return result

    # 腮红（材质名 *_constant）：底色参数指向整张脸贴图，真正要画的是
    # CheekColor + CheekMaskTexture 蒙版叠色。按普通底色贴图画会变成一条横穿脸颊的色带，
    # 还会压住眼珠（chr0103 右眼看起来像被切掉一块），所以整段直接不画。
    if material and "constant" in material.lower():
        for instance in chain:
            if "CheekColor" in instance.vectors:
                result.skip = True
                return result

    line_intensity = None
    base_done = False
    skin_mask = False
    for instance in chain:
        parent = instance.parent or ""
        if "BlendColor" in instance.vectors:
            result.blend = tuple(instance.vectors["BlendColor"][:3])
        if "AddColor" in instance.vectors:
            result.add = tuple(instance.vectors["AddColor"][:3])
        if "LineColorIntensity" in instance.vectors:
            line_intensity = tuple(instance.vectors["LineColorIntensity"][:3])
        if "RimScale" in instance.scalars:
            result.rim_scale = float(instance.scalars["RimScale"])
        if "SphereMapColorPow" in instance.scalars:
            result.sphere_pow = float(instance.scalars["SphereMapColorPow"])
        if "SphereMapBoost" in instance.scalars:
            result.sphere_boost = float(instance.scalars["SphereMapBoost"])
        if "EmissiveBoost" in instance.scalars:
            result.emissive_boost = float(instance.scalars["EmissiveBoost"])
        if "EmiParam" in instance.scalars:
            result.emissive_boost = float(instance.scalars["EmiParam"])
        if "OutlineScale" in instance.scalars or "DepthOffsetScale" in instance.scalars:
            # 反汇编里描边宽度 ∝ OutlineScale × DepthOffsetScale × 视距
            result.outline_width = max(float(instance.scalars.get("OutlineScale", 0.2))
                                       * float(instance.scalars.get("DepthOffsetScale", 0.004)),
                                       1e-5)
        if "hair" in parent.lower():
            result.threshold = 0.94
        if result.shadow == (1.0, 1.0, 1.0):
            table = (instance.textures.get("CelShadowTable")
                     or instance.textures.get("ToonShadowTable"))
            if table:
                tex = store.texture(table)
                if tex is not None:
                    img = decode_texture(tex)
                    rows = img.shape[0]
                    dark = img[max(0, int(rows * 0.02)):max(1, int(rows * 0.22)), :, :3]
                    result.shadow = tuple(float(v) / 255.0
                                          for v in dark.reshape(-1, 3).mean(axis=0))
        if result.sphere is None and not is_eye and instance.textures.get("SphereMap"):
            tex = store.texture(instance.textures["SphereMap"])
            if tex is not None:
                result.sphere = decode_texture(tex)
        if result.shadow_map is None:
            shadow_path = (instance.textures.get("ShadowTexture")
                           or instance.textures.get("ShadowMap"))
            if shadow_path:
                tex = store.texture(shadow_path)
                if tex is not None:
                    result.shadow_map = decode_texture(tex)
        for name in ("EmissiveTexture", "EmissiveColorTexture", "Emissive",
                     "emiTex", "EmiTex"):
            if result.emissive is None and instance.textures.get(name):
                tex = store.texture(instance.textures[name])
                if tex is not None:
                    result.emissive = decode_texture(tex)
        if base_done:
            continue
        picked = pick_base_color(instance.textures)
        if not picked:
            continue
        tex = store.texture(picked[1])
        if tex is None:
            continue
        image = decode_texture(tex)
        if is_outline:
            mean = image[:, :, :3].astype("f4").mean(axis=(0, 1)) / 255.0
            scale = line_intensity or (0.22, 0.22, 0.22)
            result.outline = True
            result.outline_color = tuple(
                float(min(max(mean[i] * scale[i], 0.0), 1.0)) for i in range(3))
            return result
        result.texture = image
        # 皮肤的基础色贴图是遮罩，真正的底色要用色表暗端（下面兜底）
        if "toonbasebody" in parent.lower():
            skin_mask = True
        else:
            base_done = True

    if is_outline:
        result.outline = True
        return result
    if (result.texture is None or skin_mask) and result.shadow != (1.0, 1.0, 1.0):
        pixel = np.array([[[int(result.shadow[0] * 255), int(result.shadow[1] * 255),
                            int(result.shadow[2] * 255), 255]]], dtype=np.uint8)
        result.texture = pixel
    return result


def cmd_preview(args) -> int:
    paks = _open_all(args)
    try:
        import numpy as np

        from .ue4.content import ContentStore
        from .ue4.skeletalmesh import parse_skeletal_mesh
        from .ue4.staticmesh import parse_static_mesh
        from .render import render_scene

        store = ContentStore(paks)
        package = store.package(args.asset)
        if package is None:
            raise SystemExit(f"未找到资源: {args.asset}")

        items = []
        positions = None
        label = ""
        export = package.find_export("StaticMesh")
        if export is not None:
            mesh = parse_static_mesh(package.export_bytes(export), package)
            positions = mesh.positions
            material = args.material or (mesh.material_slots[0] if mesh.material_slots else None)
            toon = ToonMaterial() if args.no_texture else _toon_params(store, material)
            if toon.texture is not None and toon.shadow != (1.0, 1.0, 1.0):
                print(f"  材质 {material}\n    阴影色 "
                      f"{tuple(round(v, 2) for v in toon.shadow)} 阈值 {toon.threshold}")
            items.append(toon.item(mesh.positions, mesh.indices, mesh.uvs))
            label = f"{len(mesh.positions)} 顶点 / {len(mesh.indices) // 3} 三角形"
        else:
            skeletal = parse_skeletal_mesh(package)
            positions = skeletal.positions
            for section in skeletal.sections:
                start = section.base_index
                count = section.num_triangles * 3
                indices = skeletal.indices[start:start + count]
                if len(indices) == 0:
                    continue
                slot = skeletal.material_slots[section.material_index] \
                    if section.material_index < len(skeletal.material_slots) else None
                toon = ToonMaterial() if args.no_texture else _toon_params(store, slot)
                toon.outline = toon.outline or bool(slot and "outline" in slot.lower())
                items.append(toon.item(skeletal.positions, indices, skeletal.uvs))
                print(f"  分段 材质槽 {section.material_index} -> {slot}")
            label = (f"{len(skeletal.positions)} 顶点 / {len(skeletal.indices) // 3} 三角形 / "
                     f"{len(skeletal.bones)} 骨骼 / {len(skeletal.sections)} 分段")

        if not any(item.texture is not None for item in items) and not args.no_texture:
            print("  [warn] 未解析到基础色贴图，使用纯色着色")

        center = (positions.min(axis=0) + positions.max(axis=0)) * 0.5
        radius = float(np.linalg.norm(positions.max(axis=0) - positions.min(axis=0)) * 0.5) or 1.0
        if args.camera:
            values = [float(v) for v in args.camera.split(",")]
            if len(values) != 6:
                raise SystemExit("--camera 需要 6 个数字: ex,ey,ez,tx,ty,tz")
            eye, target = np.array(values[:3], np.float32), np.array(values[3:], np.float32)
        else:
            target = center
            eye = center + np.array([radius * 1.9, -radius * 2.4, radius * 0.35], dtype=np.float32)

        render_scene(items, args.out, args.size, eye, target, args.exposure, args.fov,
                     light=_vec3(args.light, (0.0, 0.55, 0.83)))
        print(f"已渲染 {args.asset} -> {args.out}（{label}）")
        return 0
    finally:
        for pak in paks:
            pak.close()


def _vec3(text: str | None, default: tuple[float, float, float]):
    """解析 "x,y,z" 形式的向量参数。"""
    if not text:
        return default
    values = [float(v) for v in text.split(",")]
    if len(values) != 3:
        raise SystemExit("向量参数需要 3 个数字: x,y,z")
    return tuple(values)


def _flip_uv(uvs, mode: str):
    if uvs is None or mode in ("", "none"):
        return uvs
    import numpy as np

    out = uvs.copy()
    if "u" in mode:
        out[:, 0] = 1.0 - out[:, 0]
    if "v" in mode:
        out[:, 1] = 1.0 - out[:, 1]
    return np.asarray(out, dtype=np.float32)


def _parse_pose(specs: list[str] | None):
    """解析 --pose：骨骼名:轴:角度，轴取 x/y/z，可重复。"""
    import numpy as np

    pose = {}
    for spec in specs or []:
        parts = spec.split(":")
        if len(parts) != 3 or parts[1].lower() not in ("x", "y", "z"):
            raise SystemExit(f"--pose 需要 骨骼名:轴:角度（轴为 x/y/z），收到 {spec!r}")
        name, axis, angle = parts[0], parts[1].lower(), float(parts[2])
        angle = np.radians(angle)
        c, s = np.cos(angle), np.sin(angle)
        if axis == "x":
            rotation = np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=np.float64)
        elif axis == "y":
            rotation = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=np.float64)
        else:
            rotation = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=np.float64)
        pose[name] = rotation
    return pose


def _master_bones(store):
    """主骨架（SK_chr_all_bone）的骨骼名、绑定旋转矩阵与父索引。"""
    from .ue4.skeletalmesh import _quat_matrix, parse_skeletal_mesh

    package = store.package("Model/Character/Body/Base/chr_body_base/Mesh/SK_chr_all_bone")
    if package is None:
        raise SystemExit("未找到主骨架网格 SK_chr_all_bone")
    mesh = parse_skeletal_mesh(package)
    names = [name for name, _ in mesh.bones]
    binds = [_quat_matrix(pose[:4]) for pose in mesh.bind_pose]
    parents = [parent for _, parent in mesh.bones]
    return names, binds, parents


def _rotation_angle(matrix) -> float:
    import numpy as np

    return float(np.degrees(np.arccos(np.clip((np.trace(matrix) - 1) / 2, -1.0, 1.0))))


def _angle_between(a, b) -> float:
    """两个 3×3 旋转矩阵之间的夹角（度）。"""
    import numpy as np

    return float(np.degrees(np.arccos(np.clip((np.trace(a.T @ b) - 1) / 2, -1, 1))))


def _is_space_offset(keys, bind) -> bool:
    """判断一条旋转通道是不是「骨骼空间常量」。

    实测：5 个角色的待机动画里 AO_Neck 的键都稳定在绕 Z 约 88°，整段只变化
    10~22°，而网格里 AO_Neck 的绑定量是单位旋转。照直应用会让头转 90°（正
    面机位只能看到后脑勺）；live 动画里它的变化幅度也只有 29°，同样要按空间
    常量处理（阈值取 35° 才能覆盖两种动画）。
    """
    import numpy as np

    from .ue4.anim import _sample_keys, quat_matrix

    samples = [quat_matrix(_sample_keys(keys, f, 100000)) for f in np.linspace(0, 99999, 9)]
    deviation = float(np.mean([_angle_between(s, bind) for s in samples]))
    spread = max(_angle_between(a, b) for a in samples for b in samples)
    return deviation > 55.0 and spread < 35.0


def _facial_bones(names, parents) -> set[int]:
    """返回面部骨骼子树（GR_Facial 及其所有后代）的下标集合。

    live/commu 的身体动画里 GR_Facial 的键平均偏离绑定 107°、整段变化 119°，
    照直应用会把眼睛、眼睑转进头里（正面看脸是空白的）。面部由单独的
    *_facial_* 序列驱动，身体动画应当忽略这棵子树。
    """
    facial = set()
    for i, name in enumerate(names):
        cur = i
        for _ in range(64):
            if names[cur] == "GR_Facial" or names[cur].startswith("GR_Facial"):
                facial.add(i)
                break
            parent = parents[cur]
            if parent < 0:
                break
            cur = parent
    return facial


_REST_OFFSETS: dict | None = None
REFERENCE_IDLE = "/Game/Animation/Character/Commu/chr0101/A_chr0101_C_Wait_A_00"


def _rest_offsets(store) -> dict:
    """用参考待机动画标定每根骨骼的「静止朝向」（骨骼空间常量表）。

    取待机里最接近绑定姿势的一帧作为该骨骼的静止朝向。偏移量是骨骼空间
    常量（5 个角色的 AO_Neck 都是绕 Z 约 88°），所以一份表可以给所有动画用。
    """
    global _REST_OFFSETS
    if _REST_OFFSETS is not None:
        return _REST_OFFSETS
    _REST_OFFSETS = {}
    package = store.package(REFERENCE_IDLE)
    if package is None:
        return _REST_OFFSETS
    from .ue4.anim import AnimSequenceError, _sample_keys, parse_animation, quat_matrix
    import numpy as np

    try:
        idle = parse_animation(package)
    except AnimSequenceError:
        return _REST_OFFSETS
    names, binds, _ = _master_bones(store)

    def deviation(frame: int) -> float:
        total = 0.0
        for t, bone in enumerate(idle.bone_indices):
            bone = int(bone)
            if bone >= len(names) or len(idle.rotations[t]) < 2:
                continue
            k = quat_matrix(_sample_keys(idle.rotations[t], frame, idle.num_frames))
            total += _angle_between(k, binds[bone])
        return total

    step = max(1, idle.num_frames // 12)
    best = min(range(0, idle.num_frames, step), key=deviation)
    for t, bone in enumerate(idle.bone_indices):
        bone = int(bone)
        if bone >= len(names) or len(idle.rotations[t]) < 2:
            continue
        _REST_OFFSETS[names[bone]] = quat_matrix(
            _sample_keys(idle.rotations[t], best, idle.num_frames))
    return _REST_OFFSETS


def _anim_pose(store, anim: str, frame: int, space: bool = False) -> tuple[dict, dict]:
    """把动画某一帧解成 ({骨骼名: 局部旋转}, {骨骼名: 局部位移})。

    只用能自证的数据：
      - 键数 > 1 的旋转通道（逐个键都必须是单位四元数）；
      - 键数 > 1 的位移通道（逐帧变化才是真位移）。
    键数 = 1 的通道表示整段恒定，但实测这些通道的偏移会指进别的轨的键块
    （读出来是别根骨骼的位移，甚至 |xyz| = 92 这种非法四元数），无法自证，
    一律回退绑定姿势。
    """
    from .ue4.anim import AnimSequenceError, _sample_keys, parse_animation, quat_matrix
    import numpy as np

    package = store.package(anim)
    if package is None:
        raise SystemExit(f"未找到动画: {anim}")
    try:
        sequence = parse_animation(package)
    except AnimSequenceError as exc:
        raise SystemExit(f"动画解析失败 {anim}: {exc}") from exc

    names, binds, parents = _master_bones(store)
    facial = _facial_bones(names, parents)
    offsets_table = _rest_offsets(store) if space else {}
    rotations: dict[str, np.ndarray] = {}
    translations: dict[str, np.ndarray] = {}
    offsets = 0
    calibrated = 0
    skipped_facial = 0
    for t, bone in enumerate(sequence.bone_indices):
        bone = int(bone)
        if not 0 <= bone < len(names):
            continue
        if bone in facial:
            if len(sequence.rotations[t]) > 0:
                skipped_facial += 1
            continue
        keys = sequence.rotations[t]
        if len(keys) > 1:
            if _is_space_offset(keys, binds[bone]):
                offsets += 1
            else:
                k = quat_matrix(_sample_keys(keys, frame, sequence.num_frames))
                rest = offsets_table.get(names[bone]) if offsets_table else None
                if rest is not None:
                    # 把「静止朝向」剥掉：动作 = 绑定⁻¹×键、静止 = 绑定⁻¹×参考，再合成
                    b = binds[bone]
                    k = b @ ((b.T @ rest).T @ (b.T @ k))
                    calibrated += 1
                rotations[names[bone]] = k
        pos = sequence.positions[t]
        if len(pos) > 1:
            translations[names[bone]] = _sample_keys(pos, frame, sequence.num_frames)
    print(f"  动画 {anim.rsplit('/', 1)[-1]}：{len(sequence.bone_indices)} 轨 / "
          f"{sequence.num_frames} 帧 / 旋转自检 {sequence.quality:.2f} / "
          f"丢弃 {sequence.dropped} 轨 / 空间常量 {offsets} 轨 / "
          f"面部忽略 {skipped_facial} 轨 / 静止标定 {calibrated} 轨 / "
          f"应用 {len(rotations)} 旋转 + {len(translations)} 位移")
    return rotations, translations


def _skeletal_items(store, asset: str, no_texture: bool, flip: str = "", pose=None,
                    replace: bool = False, translations=None):
    import numpy as np

    from .ue4.skeletalmesh import parse_skeletal_mesh
    from .render import SceneItem

    package = store.package(asset)
    if package is None:
        return [], None, None
    mesh = parse_skeletal_mesh(package)
    positions = mesh.positions
    if pose:
        try:
            positions = mesh.posed_positions(pose, replace=replace, translations=translations)
        except Exception as exc:  # noqa: BLE001 - 蒙皮失败时退回绑定姿势
            print(f"  [warn] 蒙皮失败 {asset.rsplit('/', 1)[-1]}: {exc}")
    items = []
    for section in mesh.sections:
        start = section.base_index
        count = section.num_triangles * 3
        indices = mesh.indices[start:start + count]
        if len(indices) == 0:
            continue
        slot = mesh.material_slots[section.material_index] \
            if section.material_index < len(mesh.material_slots) else None
        toon = ToonMaterial() if no_texture else _toon_params(store, slot)
        if toon.skip:
            continue
        if slot and "outline" in slot.lower():
            toon.outline = True               # 描边是网格里烘焙的反向壳，剔除正面再画
        verts = positions
        uvs = _flip_uv(mesh.uvs, flip)
        if slot and slot.lower().endswith("_iris"):
            # 虹膜片和眼球表面几乎共面（实测两者最前都到 y≈7.89），斜视时会互相穿插，
            # 眼白盖掉半边虹膜（chr0103 右眼看着像瞳孔被切掉一块）。沿脸朝前推 0.5 单位。
            verts = positions.copy()
            verts[np.unique(indices), 1] += 0.5
        items.append(toon.item(verts, indices, uvs))
    return items, mesh, positions


def _head_attachment(mesh, pose, replace: bool, translations=None):
    """头部骨骼的刚性变换 + 绑定位置的平移，供头部局部空间的头发/饰品跟随。

    取 SO_Head：脸部蒙皮几乎全部绑在它上面（SC_Head/AO_Head 只是它的父级），
    用父级会让头发比脸少转几度，刘海就压到眼睛上。
    """
    import numpy as np

    names = [b[0] for b in mesh.bones]
    for bone in ("SO_Head", "AO_Head", "SC_Head"):
        if bone in names:
            i = names.index(bone)
            posed = mesh.posed_globals(pose, replace, translations)
            bind = mesh.bind_matrices()
            transform = posed[i] @ np.linalg.inv(bind[i])
            place = np.eye(4)
            place[:3, 3] = bind[i][:3, 3]
            return transform @ place
    return np.eye(4)


def _character_items(store, chr_id: str, no_texture: bool, flip: str = "", verbose: bool = True,
                     pose=None, replace: bool = False, translations=None):
    """加载身体/脸/头发/饰品（可按骨骼名摆姿势），返回 (items, 中心, 半径)。"""
    import numpy as np

    candidates = [
        f"Model/Character/Body/Priv/chr_body_priv001_{chr_id}/Mesh/SK_chr_body_priv001_{chr_id}_a",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_face",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_hair",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_orna",
    ]
    items = []
    positions = []
    head_matrix = np.eye(4, dtype=np.float64)
    for asset in candidates:
        # 头发/饰品的顶点在头部局部空间，骨骼里没有身体头部骨骼，改用头部刚性变换跟随
        attached = asset.endswith(("_hair", "_orna"))
        mesh_items, mesh, posed = _skeletal_items(
            store, asset, no_texture, flip, None if attached else pose, replace,
            None if attached else translations)
        if not mesh_items:
            continue
        if verbose:
            print(f"  {asset.rsplit('/', 1)[-1]}: {len(mesh.positions)} 顶点 / "
                  f"{len(mesh.sections)} 分段 / {len(mesh.bones)} 骨骼")
        if not attached:
            head_matrix = _head_attachment(mesh, pose, replace, translations)
        if attached:
            for item in mesh_items:
                item.model = head_matrix.astype(np.float32)
            items.extend(mesh_items)
            world = posed @ head_matrix[:3, :3].T + head_matrix[:3, 3]
            positions.append(world.astype(np.float32))
        else:
            items.extend(mesh_items)
            positions.append(posed)
    if not items:
        return [], None, 0.0

    all_positions = np.vstack(positions)
    lo, hi = all_positions.min(axis=0), all_positions.max(axis=0)
    center = (lo + hi) * 0.5
    radius = float(np.linalg.norm(hi - lo) * 0.5) or 1.0
    return items, center, radius


def cmd_character(args) -> int:
    """把一个角色的身体/脸/头发（绑定姿势）拼在一起渲染。"""
    paks = _open_all(args)
    try:
        import numpy as np

        from .ue4.content import ContentStore
        from .render import render_scene

        store = ContentStore(paks)
        chr_id = args.chr
        pose = _parse_pose(args.pose)
        replace = False
        translations = None
        if args.anim:
            pose, translations = _anim_pose(store, args.anim, args.frame,
                                            space=args.anim_space)
            replace = True
        items, center, radius = _character_items(store, chr_id, args.no_texture, args.flip_uv,
                                                 pose=pose, replace=replace,
                                                 translations=translations)
        if not items:
            raise SystemExit("没有可渲染的骨骼网格")

        if args.camera:
            values = [float(v) for v in args.camera.split(",")]
            eye, target = np.array(values[:3], np.float32), np.array(values[3:], np.float32)
        else:
            target = center
            eye = center + np.array([0.0, -radius * 2.2, radius * 0.12], dtype=np.float32)

        render_scene(items, args.out, args.size, eye, target, args.exposure, args.fov,
                     light=_vec3(args.light, (0.0, 0.55, 0.83)))
        print(f"已渲染 {chr_id} -> {args.out}")
        return 0
    finally:
        for pak in paks:
            pak.close()


def _placement_matrix(location, rotation, scale):
    import numpy as np

    pitch, yaw, roll = (float(v) for v in rotation)
    sr, cr = np.sin(np.radians(pitch)), np.cos(np.radians(pitch))
    sy, cy = np.sin(np.radians(yaw)), np.cos(np.radians(yaw))
    sp, cp = np.sin(np.radians(roll)), np.cos(np.radians(roll))
    matrix = np.eye(4, dtype=np.float32)
    matrix[:3, :3] = np.array([
        [cy * cr, cy * sr * sp - sy * cp, cy * sr * cp + sy * sp],
        [sy * cr, sy * sr * sp + cy * cp, sy * sr * cp - cy * sp],
        [-sr, cr * sp, cr * cp]], dtype=np.float32) * np.array(scale, dtype=np.float32)
    matrix[:3, 3] = np.array(location, dtype=np.float32)
    return matrix


def _is_heart_screen(mesh: str) -> bool:
    """心形大屏的屏面（SM_Stg101_Ef_V_HeartMonitor）。

    这个网格在 cooked 资产里没有材质槽，游戏里由运行时的渲染目标覆盖，
    离线预览时用中性渐变占位，避免整块屏变成默认灰。
    """
    return "heartmonitor" in (mesh or "").lower()


def _stage_items(store, map_path: str, no_texture: bool, limit: int = 0,
                 verbose: bool = True):
    """装载关卡里放置的静态网格，返回 (items, 跳过的 actor 数)。"""
    from .ue4.level import parse_level
    from .ue4.staticmesh import parse_static_mesh

    package = store.package(map_path)
    if package is None:
        raise SystemExit(f"未找到地图: {map_path}")
    placed = parse_level(package)
    if verbose:
        print(f"关卡 {map_path}：静态网格 actor {len(placed)} 个")

    mesh_cache: dict[str, object] = {}
    material_cache: dict[str, object] = {}
    items = []
    skipped = 0
    for index, entry in enumerate(placed):
        if limit and len(items) >= limit:
            break
        if entry.mesh not in mesh_cache:
            mesh_package = store.package(entry.mesh)
            if mesh_package is None:
                mesh_cache[entry.mesh] = None
            else:
                export = mesh_package.find_export("StaticMesh")
                try:
                    mesh_cache[entry.mesh] = (parse_static_mesh(
                        mesh_package.export_bytes(export), mesh_package)
                        if export else None)
                except Exception as exc:  # noqa: BLE001
                    if verbose:
                        print(f"  [warn] 网格解析失败 {entry.mesh}: {exc}")
                    mesh_cache[entry.mesh] = None
        mesh = mesh_cache[entry.mesh]
        if mesh is None:
            skipped += 1
            continue

        toon = ToonMaterial()
        placement = _placement_matrix(entry.location, entry.rotation, entry.scale)
        sections = mesh.sections or [None]
        for section in sections:
            if section is None:
                indices, slot_index = mesh.indices, 0
            else:
                start = section.first_index
                count = section.num_triangles * 3
                indices, slot_index = mesh.indices[start:start + count], section.material_index
            if len(indices) == 0:
                continue
            material = None
            if entry.materials and slot_index < len(entry.materials):
                material = entry.materials[slot_index]
            if not material and slot_index < len(mesh.material_slots):
                material = mesh.material_slots[slot_index]
            if not no_texture:
                if _is_heart_screen(entry.mesh):
                    toon = ToonMaterial(unlit=True, screen=True)
                elif material:
                    if material in material_cache:
                        toon = material_cache[material]
                    else:
                        resolved = _toon_params(store, material)
                        if (resolved.texture is None and not resolved.outline
                                and not resolved.unlit):
                            resolved.texture = _material_texture(store, material)[1]
                        material_cache[material] = resolved
                        toon = resolved
            items.append(toon.item(mesh.positions, indices, mesh.uvs, placement))
        if verbose and (index + 1) % 20 == 0:
            print(f"  ... 已装载 {index + 1}/{len(placed)}")
    return items, skipped


def cmd_scene(args) -> int:
    paks = _open_all(args)
    try:
        import numpy as np

        from .ue4.content import ContentStore
        from .render import render_scene

        store = ContentStore(paks)
        items, skipped = _stage_items(store, args.map, args.no_texture, args.limit)
        if not items:
            raise SystemExit("没有可渲染的静态网格")

        character_center = None
        character_radius = 0.0
        if args.character:
            pose = _parse_pose(args.pose)
            replace = False
            translations = None
            if args.anim:
                pose, translations = _anim_pose(store, args.anim, args.frame,
                                                space=args.anim_space)
                replace = True
            char_items, character_center, character_radius = _character_items(
                store, args.character, args.no_texture, args.flip_uv, verbose=False,
                pose=pose, replace=replace, translations=translations)
            if not char_items:
                raise SystemExit(f"没有可渲染的角色: {args.character}")
            items.extend(char_items)
            print(f"  角色 {args.character}：{len(char_items)} 个分段")

        eye = target = None
        if args.camera:
            values = [float(v) for v in args.camera.split(",")]
            if len(values) != 6:
                raise SystemExit("--camera 需要 6 个数字: ex,ey,ez,tx,ty,tz")
            eye = np.array(values[:3], dtype=np.float32)
            target = np.array(values[3:], dtype=np.float32)

        shadow_kwargs = {}
        if character_center is not None:
            # 阴影图聚焦到角色附近，避免被整个舞台摊薄
            shadow_kwargs = {"shadow_center": character_center,
                             "shadow_radius": max(character_radius * 1.6, 120.0)}
        render_scene(items, args.out, args.size, eye, target, args.exposure, args.fov,
                     light=_vec3(args.light, (0.35, 0.45, 0.75)), **shadow_kwargs)
        print(f"已渲染 {len(items)} 个网格 -> {args.out}（跳过 {skipped} 个）")
        return 0
    finally:
        for pak in paks:
            pak.close()


def _parse_views(specs: list[str] | None):
    """解析 --view：name:ex,ey,ez,tx,ty,tz。"""
    views = []
    for spec in specs or []:
        name, _, numbers = spec.partition(":")
        try:
            values = [float(v) for v in numbers.split(",")]
        except ValueError:
            values = []
        if len(values) != 6:
            raise SystemExit(f"--view 需要 name:ex,ey,ez,tx,ty,tz，收到 {spec!r}")
        views.append((name or f"view{len(views) + 1}", values[:3], values[3:]))
    return views


def cmd_batch(args) -> int:
    """一次输出多角色 × 多机位的对照图。"""
    paks = _open_all(args)
    try:
        import numpy as np

        from .ue4.content import ContentStore
        from .render import render_scene

        store = ContentStore(paks)
        stage_items, _ = _stage_items(store, args.map, args.no_texture, args.limit,
                                      verbose=False)
        if not stage_items:
            raise SystemExit("没有可渲染的静态网格")

        characters = [c.strip() for c in args.characters.split(",") if c.strip()]
        views = _parse_views(args.view)
        os.makedirs(args.out, exist_ok=True)
        light = _vec3(args.light, (0.35, 0.45, 0.75))
        shots = 0
        for chr_id in characters:
            char_items, center, radius = _character_items(
                store, chr_id, args.no_texture, args.flip_uv, verbose=False)
            if not char_items:
                print(f"  [warn] {chr_id} 没有可渲染的网格")
                continue
            shadow = {"shadow_center": center, "shadow_radius": max(radius * 1.6, 120.0)}
            # 没给机位时按角色包围盒自动取正视与四分之三侧视
            cameras = views or [
                ("front", center + np.array([0.0, -radius * 2.2, radius * 0.12],
                                            dtype=np.float32), center),
                ("threequarter", center + np.array([radius * 1.6, -radius * 1.8,
                                                    radius * 0.5], dtype=np.float32), center),
            ]
            for name, eye, target in cameras:
                path = os.path.join(args.out, f"{chr_id}_{name}.png")
                render_scene(stage_items + char_items, path, args.size,
                             np.array(eye, dtype=np.float32),
                             np.array(target, dtype=np.float32),
                             args.exposure, args.fov, light=light, **shadow)
                print(f"  {chr_id} / {name} -> {path}")
                shots += 1
        print(f"已输出 {shots} 张对照图 -> {args.out}")
        return 0
    finally:
        for pak in paks:
            pak.close()


def _material_parameters(package) -> list[str]:
    """材质表达式里的 ParameterName（RDEF 被剥离后，参数名只能从材质资产里取）。"""
    from .ue4.properties import name_text, parse_properties

    names: list[str] = []
    for export in package.exports:
        if not export.class_name.startswith("MaterialExpression"):
            continue
        try:
            data = package.export_bytes(export)
            props, _ = parse_properties(data, package.names)
        except Exception:  # noqa: BLE001 - 单个表达式解析失败不影响其它导出
            continue
        for prop in props:
            if prop.name == "ParameterName" and prop.type == "NameProperty":
                text = name_text(data, prop, package.names)
                if text not in names:
                    names.append(text)
    return names


def cmd_live_bundle(args) -> int:
    """把 LevelSequence 的相机轨道导出成 liveview 验证程序可直接读的文本包。"""
    from .ue4.sequence import camera_pose, parse_level_sequence

    paks = _open_all(args)
    try:
        from .ue4.content import ContentStore

        store = ContentStore(paks)
        package = store.package(args.asset)
        if package is None:
            raise SystemExit(f"未找到资源: {args.asset}")
        info = parse_level_sequence(package)
        binding = None
        if args.binding:
            binding = next((b for b in info.bindings if b.name == args.binding), None)
        if binding is None:
            binding = next((b for b in info.bindings
                            if any(t.class_name == "LiveCameraMovementTrack"
                                   for t in b.tracks)), info.bindings[0])
        start = args.start if args.start is not None else (info.play_start or 0)
        end = args.end if args.end is not None else (info.play_end or 0)
        step = max(1, args.step)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        written = 0
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write("#liveview-bundle 1\n")
            handle.write(f"#asset {args.asset}\n")
            handle.write(f"#binding {binding.name}\n")
            handle.write(f"#tick {info.tick_fps:g}\n")
            handle.write(f"#range {start} {end} {step}\n")
            for frame in range(start, end + 1, step):
                pose = camera_pose(binding, frame)
                if pose is None:
                    continue
                pos, aim = pose.position, pose.target
                handle.write(f"F {frame} {pos[0]:.4f} {pos[1]:.4f} {pos[2]:.4f} "
                             f"{aim[0]:.4f} {aim[1]:.4f} {aim[2]:.4f}\n")
                written += 1
        print(f"已写出 {written} 帧机位 -> {args.out}（绑定 {binding.name}）")
        return 0
    finally:
        for pak in paks:
            pak.close()


def _shrink_texture(image, limit: int):
    """把贴图最长边缩到 limit（RGBA8，行序自上而下）——块平均，不依赖 PIL。"""
    import numpy as np

    if image is None:
        return None
    pixels = np.ascontiguousarray(image[:, :, :4], dtype=np.uint8)
    height, width = pixels.shape[:2]
    step = 1
    while max(width // step, height // step) > limit:
        step *= 2
    if step == 1:
        return pixels
    usable_h = (height // step) * step
    usable_w = (width // step) * step
    blocks = pixels[:usable_h, :usable_w].astype(np.uint16).reshape(
        usable_h // step, step, usable_w // step, step, 4)
    return blocks.mean(axis=(1, 3)).astype(np.uint8)


class ShowMaterialLibrary:
    """把材质名解析成 Toon 参数 + 贴图表下标。

    同名材质与同内容贴图只存一份：舞台与角色共用一张表，索引在演出包里通用。
    """

    def __init__(self, store):
        self.store = store
        self.textures: list = []
        self.materials: list = []
        self._material_index: dict[tuple, int] = {}
        self._texture_index: dict[tuple, int] = {}

    def add_texture(self, image, limit: int) -> int:
        import hashlib
        import numpy as np

        if image is None:
            return -1
        image = _shrink_texture(image, limit)
        key = (limit, hashlib.blake2b(np.ascontiguousarray(image).tobytes(),
                                      digest_size=16).digest())
        if key in self._texture_index:
            return self._texture_index[key]
        self.textures.append(image)
        index = len(self.textures) - 1
        self._texture_index[key] = index
        return index

    def resolve(self, material: str | None, fallback_color=(0.78, 0.76, 0.72),
                limit: int = 1024) -> int:
        """返回材质下标；-1 表示这一层不画（腮红等纯叠色层）。"""
        import numpy as np

        from .ue4.show import BLEND_MODES, MaterialData

        key = (material or "", limit)
        if key in self._material_index:
            return self._material_index[key]

        try:
            toon = _toon_params(self.store, material) if material else ToonMaterial()
        except Exception:  # noqa: BLE001 - 单个材质解析失败不该毁掉整包导出
            toon = ToonMaterial()
        if toon.skip:
            self._material_index[key] = -1
            return -1

        if toon.outline:
            # 描边材质在网格里是外扩壳，只刷一遍线色
            pixel = np.full((1, 1, 4), 255, np.uint8)
            pixel[0, 0, :3] = [int(min(max(c, 0.0), 1.0) * 255.0) for c in toon.outline_color]
            data = MaterialData(base_texture=self.add_texture(pixel, limit),
                                unlit=True, outline=True,
                                outline_width=float(toon.outline_width))
        else:
            base = self.add_texture(toon.texture, limit)
            if base < 0:
                pixel = np.full((1, 1, 4), 255, np.uint8)
                pixel[0, 0, :3] = [int(min(max(c, 0.0), 1.0) * 255.0)
                                   for c in fallback_color]
                base = self.add_texture(pixel, limit)
            data = MaterialData(
                base_texture=base,
                sphere_texture=self.add_texture(toon.sphere, limit),
                shadow_texture=self.add_texture(toon.shadow_map, limit),
                shadow_tint=tuple(float(v) for v in toon.shadow),
                threshold=float(toon.threshold),
                blend=tuple(float(v) for v in toon.blend),
                add=tuple(float(v) for v in toon.add),
                rim_scale=float(toon.rim_scale),
                sphere_blend=tuple(float(v) for v in toon.sphere_blend),
                sphere_pow=float(toon.sphere_pow),
                sphere_boost=float(toon.sphere_boost),
                blend_mode=BLEND_MODES.get(toon.blend_mode, 0),
                unlit=bool(toon.unlit))
        self.materials.append(data)
        index = len(self.materials) - 1
        self._material_index[key] = index
        return index


def _show_stage_meshes(store, map_path: str, library: ShowMaterialLibrary,
                       verbose: bool = True):
    """把关卡里的静态网格烘成世界坐标的演出网格（带 UV 与分段材质）。"""
    import numpy as np

    from .ue4.level import parse_level
    from .ue4.show import MeshSection, StageMesh
    from .ue4.staticmesh import parse_static_mesh
    from .render import _smooth_normals

    package = store.package(map_path)
    if package is None:
        raise SystemExit(f"未找到地图: {map_path}")
    placed = parse_level(package)
    if verbose:
        print(f"关卡 {map_path}：静态网格 actor {len(placed)} 个")

    cache: dict[str, object] = {}
    meshes = []
    skipped = 0
    for entry in placed:
        if entry.mesh not in cache:
            mesh_package = store.package(entry.mesh)
            mesh = None
            if mesh_package is not None:
                export = mesh_package.find_export("StaticMesh")
                if export is not None:
                    try:
                        mesh = parse_static_mesh(mesh_package.export_bytes(export),
                                                 mesh_package)
                    except Exception:  # noqa: BLE001
                        mesh = None
            cache[entry.mesh] = mesh
        mesh = cache[entry.mesh]
        if mesh is None or len(mesh.indices) == 0:
            skipped += 1
            continue

        placement = _placement_matrix(entry.location, entry.rotation, entry.scale)
        rotation = placement[:3, :3]
        positions = (mesh.positions @ rotation.T + placement[:3, 3]).astype(np.float32)
        normals = _smooth_normals(mesh.positions, mesh.indices)
        normal_matrix = np.linalg.inv(rotation).T
        normals = normals @ normal_matrix.T
        length = np.linalg.norm(normals, axis=1, keepdims=True)
        length[length == 0] = 1.0
        normals = (normals / length).astype(np.float32)

        kept: list[np.ndarray] = []
        sections: list = []
        index_at = 0
        for section in mesh.sections or [None]:
            if section is None:
                indices, slot_index = mesh.indices, 0
            else:
                start = section.first_index
                count = section.num_triangles * 3
                indices, slot_index = mesh.indices[start:start + count], section.material_index
            if len(indices) == 0:
                continue
            material = None
            if entry.materials and slot_index < len(entry.materials):
                material = entry.materials[slot_index]
            if not material and slot_index < len(mesh.material_slots):
                material = mesh.material_slots[slot_index]
            # 舞台贴图只用 256 边长：构件多，贴图数量远比角色大
            material_index = library.resolve(material, (0.55, 0.55, 0.6), limit=256)
            if material_index < 0:
                continue
            kept.append(indices)
            count = int(len(indices))
            sections.append(MeshSection(index_at, count, material_index))
            index_at += count
        if not kept:
            continue
        indices = np.concatenate(kept).astype(np.uint32)
        meshes.append(StageMesh(positions=positions, normals=normals, indices=indices,
                                uvs=mesh.uvs, sections=sections))
    if verbose:
        total = sum(len(m.positions) for m in meshes)
        print(f"  舞台网格 {len(meshes)} 个 / 顶点 {total:,}（跳过 {skipped} 个 actor）")
    return meshes


def _show_skin_meshes(store, chr_id: str, library: ShowMaterialLibrary,
                      verbose: bool = True):
    """把角色的身体/脸/头发/饰品导成蒙皮网格（头发与饰品跟随 SO_Head）。"""
    import numpy as np

    from .ue4.show import MeshSection, SkinMesh
    from .ue4.skeletalmesh import parse_skeletal_mesh
    from .render import _smooth_normals

    candidates = [
        f"Model/Character/Body/Priv/chr_body_priv001_{chr_id}/Mesh/SK_chr_body_priv001_{chr_id}_a",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_face",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_hair",
        f"Model/Character/Chr/{chr_id}/Mesh/SK_chr_{chr_id}_orna",
    ]
    meshes = []
    for asset in candidates:
        package = store.package(asset)
        if package is None:
            continue
        try:
            mesh = parse_skeletal_mesh(package)
        except Exception as exc:  # noqa: BLE001
            if verbose:
                print(f"  [warn] 骨骼网格解析失败 {asset.rsplit('/', 1)[-1]}: {exc}")
            continue
        if mesh.influence_bones is None or mesh.influence_weights is None:
            if verbose:
                print(f"  [warn] {asset.rsplit('/', 1)[-1]} 缺少蒙皮权重，跳过")
            continue

        normals = _smooth_normals(mesh.positions, mesh.indices)
        kept: list[np.ndarray] = []
        sections: list = []
        index_at = 0
        for section in mesh.sections:
            start = section.base_index
            count = section.num_triangles * 3
            indices = mesh.indices[start:start + count]
            if len(indices) == 0:
                continue
            slot = (mesh.material_slots[section.material_index]
                    if section.material_index < len(mesh.material_slots) else None)
            material_index = library.resolve(slot, (0.62, 0.58, 0.55), limit=1024)
            if material_index < 0:
                continue
            kept.append(indices)
            count = int(len(indices))
            sections.append(MeshSection(index_at, count, material_index))
            index_at += count
        if not kept:
            continue
        indices = np.concatenate(kept).astype(np.uint32)
        attach = "SO_Head" if asset.endswith(("_hair", "_orna")) else ""
        # 权重为 0 的槽位里可能残留 0xFFFF（无影响的哨兵值），钳到 0 号骨骼，
        # 否则 C++ 侧按骨骼下标取矩阵会越界。
        bone_indices = mesh.influence_bones.astype(np.int32).copy()
        weights = mesh.influence_weights.astype(np.float32).copy()
        bone_indices[weights <= 0.0] = 0
        meshes.append(SkinMesh(
            bones=mesh.bones, bind_pose=mesh.bind_pose,
            positions=mesh.positions, normals=normals,
            bone_indices=bone_indices.astype(np.uint16),
            weights=weights,
            indices=indices, attach_bone=attach,
            uvs=mesh.uvs, sections=sections))
        if verbose:
            print(f"  {asset.rsplit('/', 1)[-1]}: {len(mesh.positions)} 顶点 / "
                  f"{len(indices) // 3} 三角 / {len(mesh.bones)} 骨骼")
    return meshes


def _show_animation(store, anim: str, step: int, verbose: bool = True):
    """逐帧采样动画，返回 (Animation, 原始总帧数, 帧率)。"""
    import numpy as np

    from .ue4.anim import _sample_keys, parse_animation, quat_matrix
    from .ue4.show import Animation, quat_from_matrix

    package = store.package(anim)
    if package is None:
        raise SystemExit(f"未找到动画: {anim}")
    try:
        sequence = parse_animation(package)
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"动画解析失败 {anim}: {exc}") from exc

    names, binds, parents = _master_bones(store)
    facial = _facial_bones(names, parents)
    tracks: list[tuple[str, np.ndarray | None, np.ndarray | None]] = []
    seen: set[str] = set()
    space_offsets = 0
    for t, bone in enumerate(sequence.bone_indices):
        bone = int(bone)
        if not 0 <= bone < len(names) or bone in facial:
            continue
        name = names[bone]
        if name in seen:
            continue
        rotation = None
        keys = sequence.rotations[t]
        if len(keys) > 1:
            if _is_space_offset(keys, binds[bone]):
                space_offsets += 1
            else:
                rotation = keys
        position = sequence.positions[t] if len(sequence.positions[t]) > 1 else None
        if rotation is None and position is None:
            continue
        seen.add(name)
        tracks.append((name, rotation, position))

    step = max(1, step)
    frames = list(range(0, sequence.num_frames, step))
    if frames[-1] != sequence.num_frames - 1:
        frames.append(sequence.num_frames - 1)

    quats = np.zeros((len(frames), len(tracks), 4), np.float32)
    quats[:, :, 3] = 1.0
    has_rotation = np.zeros((len(frames), len(tracks)), bool)
    translations = np.full((len(frames), len(tracks), 3), np.nan, np.float32)
    for row, frame in enumerate(frames):
        for column, (_, rotation, position) in enumerate(tracks):
            if rotation is not None:
                quats[row, column] = quat_from_matrix(
                    quat_matrix(_sample_keys(rotation, frame, sequence.num_frames)))
                has_rotation[row, column] = True
            if position is not None:
                translations[row, column] = _sample_keys(
                    position, frame, sequence.num_frames)
    if verbose:
        print(f"  动画 {anim.rsplit('/', 1)[-1]}：{len(tracks)} 根骨骼 / "
              f"{sequence.num_frames} 帧 -> 采样 {len(frames)} 帧 "
              f"（空间常量 {space_offsets} 轨）")
    animation = Animation(bone_names=[t[0] for t in tracks], frames=frames,
                          quats=quats, has_rotation=has_rotation,
                          translations=translations)
    return animation, sequence.num_frames


def _show_camera_keys(store, sequence_asset: str, binding_name: str,
                      duration: float, fps: float, character_binding: str = ""):
    """按固定帧率采样相机轨，并顺带取角色站位（角色相机的瞄准点）。"""
    from .ue4.sequence import camera_pose, parse_level_sequence

    package = store.package(sequence_asset)
    if package is None:
        raise SystemExit(f"未找到序列: {sequence_asset}")
    info = parse_level_sequence(package)
    binding = next((b for b in info.bindings if b.name == binding_name), None)
    if binding is None:
        binding = next((b for b in info.bindings
                        if any(t.class_name == "LiveCameraMovementTrack" for t in b.tracks)),
                       info.bindings[0] if info.bindings else None)
    if binding is None:
        raise SystemExit(f"序列里没有相机绑定: {sequence_asset}")
    tick_fps = info.tick_fps or info.display_fps or 1.0

    # 角色站位：角色跟拍相机（CamCharaA）的环绕中心就是该角色在舞台上的位置
    character_target = None
    if character_binding:
        character = next((b for b in info.bindings if b.name == character_binding), None)
        if character is not None:
            pose = camera_pose(character, 0.0)
            if pose is not None:
                character_target = pose.target

    keys = []
    count = max(1, int(round(duration * fps)) + 1)
    for i in range(count):
        frame = (i / fps) * tick_fps
        pose = camera_pose(binding, frame)
        if pose is None:
            continue
        keys.append((int(round(frame)), pose.position, pose.target))
    return keys, tick_fps, binding.name, character_target


def cmd_live_show(args) -> int:
    """导出 liveview 直接播放的演出包（相机 + 舞台 + 角色 + 动画）。"""
    paks = _open_all(args)
    try:
        from .ue4.content import ContentStore
        from .ue4.show import ShowWriter

        store = ContentStore(paks)
        anim_fps = float(args.anim_fps)
        animation, num_frames = _show_animation(store, args.anim, args.anim_step)
        duration = max(0.0, (num_frames - 1) / anim_fps)
        camera_keys, tick_fps, binding_name, character_target = _show_camera_keys(
            store, args.sequence, args.binding, duration, args.camera_fps,
            args.character_binding)
        library = ShowMaterialLibrary(store)
        stages = _show_stage_meshes(store, args.map, library)
        skins = _show_skin_meshes(store, args.chr, library)

        # 角色落位：优先用角色跟拍相机的瞄准点（= 角色在舞台上的位置），
        # 没有该绑定时退回播放相机的首个瞄准点。脚底贴到主地面（实测 z≈0.1）。
        anchor = character_target
        if anchor is None and camera_keys:
            anchor = camera_keys[0][2]
        if anchor is not None:
            offset = (float(anchor[0]), float(anchor[1]), float(args.character_z))
        else:
            offset = (0.0, 0.0, float(args.character_z))

        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        writer = ShowWriter(args.out)
        try:
            writer.timeline(tick_fps, anim_fps, duration, offset)
            writer.camera(camera_keys)
            writer.textures(library.textures)
            writer.materials(library.materials)
            writer.stage(stages)
            writer.skins(skins)
            writer.animation(animation)
        finally:
            writer.close()
        print(f"已写出演出包 {args.out}")
        print(f"  时长 {duration:.2f}s  相机 {binding_name} {len(camera_keys)} 关键帧  "
              f"舞台 {len(stages)} 网格  蒙皮 {len(skins)} 网格  "
              f"动画 {len(animation.bone_names)} 骨骼 × {len(animation.frames)} 帧  "
              f"材质 {len(library.materials)} 个  贴图 {len(library.textures)} 张")
        return 0
    finally:
        for pak in paks:
            pak.close()


def cmd_sequence(args) -> int:
    """列出 LevelSequence 的绑定、轨道与区段范围。"""
    from .ue4.sequence import (SequenceError, camera_pose, parse_level_sequence,
                               sample_section)

    paks = _open_all(args)
    try:
        from .ue4.content import ContentStore

        store = ContentStore(paks)
        package = store.package(args.asset)
        if package is None:
            raise SystemExit(f"未找到资源: {args.asset}")
        try:
            info = parse_level_sequence(package)
        except SequenceError as exc:
            raise SystemExit(str(exc))
        start = "∞" if info.play_start is None else f"{info.play_start}"
        end = "∞" if info.play_end is None else f"{info.play_end}"
        seconds = ""
        if info.play_start is not None and info.play_end is not None and info.tick_fps:
            seconds = f"（{(info.play_end - info.play_start) / info.tick_fps:.2f}s）"
        print(f"播放范围 {start} .. {end} 帧{seconds}   "
              f"tick={info.tick_fps:g}fps display={info.display_fps:g}fps")
        print(f"占位对象 {len(info.possessables)} 个，绑定 {len(info.bindings)} 个")
        if args.camera_frame is not None:
            print(f"\n第 {args.camera_frame} 帧机位：")
            for binding in info.bindings:
                pose = camera_pose(binding, args.camera_frame)
                if pose is None:
                    continue
                pos, aim = pose.position, pose.target
                print(f"  [{binding.name}] \"{pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f},"
                      f"{aim[0]:.2f},{aim[1]:.2f},{aim[2]:.2f}\"")
            return 0
        if args.sample is not None:
            print(f"\n第 {args.sample} 帧采样：")
            for binding in info.bindings[:4]:
                for track in binding.tracks:
                    for section in track.sections:
                        values = sample_section(section, args.sample)
                        if not values:
                            continue
                        shown = ", ".join(f"{k}={v:.2f}"
                                          for k, v in list(values.items())[:6])
                        print(f"  [{binding.name}] {track.class_name}: {shown}")
            return 0
        for binding in info.bindings:
            classes = sorted({track.class_name for track in binding.tracks})
            print(f"  [{binding.name}] 轨道 {len(binding.tracks)} 条 {classes}")
            for track in binding.tracks[:args.tracks]:
                spans = ", ".join(
                    f"{'∞' if s.start is None else s.start}..{'∞' if s.end is None else s.end}"
                    for s in track.sections) or "无"
                print(f"      {track.class_name} 区段 {len(track.sections)} {spans}")
                for section in track.sections:
                    keys = sum(len(v) for v in section.channels.values())
                    named = ", ".join(f"{k}({len(v)})"
                                      for k, v in list(section.channels.items())[:4])
                    print(f"         通道 {len(section.channels)} 个 / 关键帧 {keys} "
                          f"{named}")
        return 0
    finally:
        for pak in paks:
            pak.close()


def cmd_shaders(args) -> int:
    """导出材质包里的 SM5 着色器：RDEF 摘要 + 反汇编全文。"""
    paks = _open_all(args)
    try:
        from .ue4.content import ContentStore
        from .ue4.shader import ShaderError, find_shaders

        store = ContentStore(paks)
        package = store.package(args.asset)
        if package is None:
            raise SystemExit(f"未找到资源: {args.asset}")

        shaders = sorted(find_shaders(package), key=lambda item: item.offset)
        if args.max:
            shaders = shaders[:args.max]
        if not shaders:
            raise SystemExit(f"{args.asset} 里没有找到 DXBC 着色器")

        dump: list[str] = [
            f"资源 {args.asset}",
            f"uexp {len(package.uexp):,} 字节，共 {len(shaders)} 个着色器（按 zlib 流偏移排序）",
        ]
        parameters = _material_parameters(package)
        if parameters:
            dump.append("材质参数名（来自材质表达式，RDEF 已被剥离）: " + ", ".join(parameters))
        dump.append("")
        for index, shader in enumerate(shaders, 1):
            dump.append(f"=== #{index} {shader.version}  {shader.source}@{shader.offset}  "
                        f"{len(shader.data)} 字节 ===")

            rdef = shader.container.rdef
            if rdef.buffers or rdef.resources:
                dump.append("[RDEF] " + shader.container.describe_rdef().replace("\n", "\n        "))
            else:
                dump.append("[RDEF] 缺失（UE4 cook 时剥离了反射数据，没有缓冲/参数名）")

            try:
                decl = shader.container.declarations()
                has_sample = any(inst.is_sample for inst in shader.instructions())
                text = shader.disassemble()
            except ShaderError as exc:
                dump.append(f"[反汇编] {exc}")
                dump.append("")
                print(f"[{index}] {shader.version}  反汇编失败：{exc}")
                continue

            if decl["cb"]:
                dump.append("[常量缓冲] " + " ".join(decl["cb"]))
            if decl["t"] or decl["u"]:
                dump.append("[贴图资源] " + " ".join(decl["t"] +
                                                    [f"{name}(uav)" for name in decl["u"]]))
            if decl["s"]:
                dump.append("[采样器] " + " ".join(decl["s"]))
            buffers = shader.uniform_buffer_names
            if buffers:
                dump.append("[参数表] uniform buffer（按 cb 顺序）: " + ", ".join(buffers))
            dump.append("[反汇编]")
            dump.append(text)
            dump.append("")

            resources = decl["t"] + decl["u"]
            print(f"[{index}] {shader.version}  {shader.source}@{shader.offset}  "
                  f"指令 {len(text.splitlines()) - 1} 条  "
                  f"资源 {','.join(resources) if resources else '无'}  "
                  f"采样 {'有' if has_sample else '无'}")

        out_dir = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(out_dir, exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write("\n".join(dump))
        print(f"已写出 {args.out}")
        return 0
    finally:
        for pak in paks:
            pak.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="sspak", description="StarLit Season pak 工具")
    parser.add_argument("--paks", help="pak 所在目录")
    parser.add_argument("--pak", action="append", help="单个 pak（可重复）")
    parser.add_argument("--key", help="AES-256 密钥（十六进制）")
    parser.add_argument("--include-mods", action="store_true", help="包含 ~mods 中的 pak")

    sub = parser.add_subparsers(dest="command", required=True)

    p_verify = sub.add_parser("verify", help="解析索引并抽样提取校验")
    p_verify.add_argument("--sample", type=int, default=40, help="每个 pak 抽样条数")
    p_verify.add_argument("--full", action="store_true", help="完整解压并校验长度")
    p_verify.set_defaults(func=cmd_verify)

    p_list = sub.add_parser("list", help="列出条目")
    p_list.add_argument("--filter", help="名称包含关键字")
    p_list.add_argument("--limit", type=int, default=0)
    p_list.set_defaults(func=cmd_list)

    p_extract = sub.add_parser("extract", help="提取单个条目")
    p_extract.add_argument("--name", required=True)
    p_extract.add_argument("--out", required=True)
    p_extract.set_defaults(func=cmd_extract)

    p_census = sub.add_parser("census", help="资产普查")
    p_census.add_argument("--headers", action="store_true", help="解析全部 uasset 包头")
    p_census.set_defaults(func=cmd_census)

    p_preview = sub.add_parser("preview", help="渲染 StaticMesh 预览图")
    p_preview.add_argument("--asset", required=True, help="资源路径（不含扩展名）")
    p_preview.add_argument("--out", required=True, help="输出 PNG 路径")
    p_preview.add_argument("--size", type=int, default=1024)
    p_preview.add_argument("--material", help="指定材质路径（默认取第一个材质槽）")
    p_preview.add_argument("--camera", help="ex,ey,ez,tx,ty,tz")
    p_preview.add_argument("--fov", type=float, default=45.0)
    p_preview.add_argument("--exposure", type=float, default=1.0,
                           help="固定曝光（关卡 PostProcess 为 1.0）")
    p_preview.add_argument("--light", help="光照方向 x,y,z（指向光源）")
    p_preview.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_preview.set_defaults(func=cmd_preview)

    p_scene = sub.add_parser("scene", help="渲染关卡中的静态网格组合画面")
    p_scene.add_argument("--map", required=True, help="地图资源路径（不含扩展名）")
    p_scene.add_argument("--out", required=True, help="输出 PNG 路径")
    p_scene.add_argument("--size", type=int, default=1024)
    p_scene.add_argument("--fov", type=float, default=45.0)
    p_scene.add_argument("--camera", help="ex,ey,ez,tx,ty,tz")
    p_scene.add_argument("--exposure", type=float, default=1.0,
                         help="固定曝光（关卡 PostProcess 为 1.0）")
    p_scene.add_argument("--light", help="光照方向 x,y,z（指向光源）")
    p_scene.add_argument("--limit", type=int, default=0, help="最多装载多少个网格")
    p_scene.add_argument("--character", help="把角色（绑定姿势）放进场景，例如 chr0101")
    p_scene.add_argument("--pose", action="append",
                         help="按骨骼名摆角色姿势：骨骼名:轴:角度（轴为 x/y/z），可重复")
    p_scene.add_argument("--anim", help="实验性：把角色摆成 AnimSequence 的某一帧")
    p_scene.add_argument("--anim-space", action="store_true",
                         help="用参考待机标定每根骨骼的静止朝向，剥掉骨骼空间常量后再摆姿势")
    p_scene.add_argument("--frame", type=int, default=0, help="--anim 的帧号")
    p_scene.add_argument("--flip-uv", default="", choices=["", "none", "u", "v", "uv"],
                         help="翻转 UV 轴，用于排查贴图方向")
    p_scene.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_scene.set_defaults(func=cmd_scene)

    p_batch = sub.add_parser("batch", help="批量输出多角色 × 多机位的对照图")
    p_batch.add_argument("--map", required=True, help="地图资源路径（不含扩展名）")
    p_batch.add_argument("--out", required=True, help="输出 PNG 的目录")
    p_batch.add_argument("--characters", default="chr0101", help="角色 ID，逗号分隔")
    p_batch.add_argument("--view", action="append",
                         help="机位 name:ex,ey,ez,tx,ty,tz（可重复；缺省用自动机位）")
    p_batch.add_argument("--size", type=int, default=1024)
    p_batch.add_argument("--fov", type=float, default=45.0)
    p_batch.add_argument("--exposure", type=float, default=1.0,
                         help="固定曝光（关卡 PostProcess 为 1.0）")
    p_batch.add_argument("--light", help="光照方向 x,y,z（指向光源）")
    p_batch.add_argument("--limit", type=int, default=0, help="最多装载多少个网格")
    p_batch.add_argument("--flip-uv", default="", choices=["", "none", "u", "v", "uv"],
                         help="翻转 UV 轴，用于排查贴图方向")
    p_batch.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_batch.set_defaults(func=cmd_batch)

    p_character = sub.add_parser("character", help="渲染角色的身体/脸/头发（绑定姿势）")
    p_character.add_argument("--chr", default="chr0101", help="角色 ID，例如 chr0101")
    p_character.add_argument("--out", required=True, help="输出 PNG 路径")
    p_character.add_argument("--size", type=int, default=900)
    p_character.add_argument("--fov", type=float, default=40.0)
    p_character.add_argument("--camera", help="ex,ey,ez,tx,ty,tz")
    p_character.add_argument("--exposure", type=float, default=1.0,
                             help="固定曝光（关卡 PostProcess 为 1.0）")
    p_character.add_argument("--light", help="光照方向 x,y,z（指向光源）")
    p_character.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_character.add_argument("--pose", action="append",
                             help="按骨骼名摆姿势：骨骼名:轴:角度（轴为 x/y/z），可重复")
    p_character.add_argument("--anim",
                             help="实验性：用 AnimSequence 的某一帧摆姿势")
    p_character.add_argument("--anim-space", action="store_true",
                             help="用参考待机标定每根骨骼的静止朝向，剥掉骨骼空间常量后再摆姿势")
    p_character.add_argument("--frame", type=int, default=0, help="--anim 的帧号")
    p_character.add_argument("--flip-uv", default="", choices=["", "none", "u", "v", "uv"],
                             help="翻转 UV 轴，用于排查贴图方向")
    p_character.set_defaults(func=cmd_character)

    p_shaders = sub.add_parser("shaders", help="导出材质里的 SM5 着色器反汇编")
    p_shaders.add_argument("--asset", required=True, help="材质路径（不含扩展名）")
    p_shaders.add_argument("--out", required=True, help="输出 TXT 路径")
    p_shaders.add_argument("--max", type=int, default=0, help="最多导出多少个着色器")
    p_shaders.set_defaults(func=cmd_shaders)

    p_sequence = sub.add_parser("sequence", help="列出 LevelSequence 的绑定、轨道与区段")
    p_sequence.add_argument("--asset", required=True, help="序列资源路径（不含扩展名）")
    p_sequence.add_argument("--tracks", type=int, default=6, help="每个绑定最多列出多少条轨道")
    p_sequence.add_argument("--sample", type=int, default=None,
                            help="在指定帧采样通道值（线性插值）")
    p_sequence.add_argument("--camera-frame", type=int, default=None,
                            help="在指定帧输出各相机绑定的机位（可直接喂给 --camera）")
    p_sequence.set_defaults(func=cmd_sequence)

    p_bundle = sub.add_parser("live-bundle", help="导出 liveview 验证程序用的相机轨包")
    p_bundle.add_argument("--asset", required=True, help="LevelSequence 资源路径（不含扩展名）")
    p_bundle.add_argument("--out", required=True, help="输出文本包路径")
    p_bundle.add_argument("--binding", help="相机绑定名（默认取第一个带 Movement 轨道的）")
    p_bundle.add_argument("--start", type=int, default=None, help="起始帧")
    p_bundle.add_argument("--end", type=int, default=None, help="结束帧")
    p_bundle.add_argument("--step", type=int, default=240, help="采样步长（帧）")
    p_bundle.set_defaults(func=cmd_live_bundle)

    p_show = sub.add_parser("live-show", help="导出 liveview 直接播放的演出包（相机+舞台+角色）")
    p_show.add_argument("--out", required=True, help="输出演出包路径（*.lvs）")
    p_show.add_argument("--chr", default="chr0101", help="角色 ID")
    p_show.add_argument("--anim", default="Animation/Character/Live/Sng001/A_sng001_D_Pos01",
                        help="舞蹈动画资源路径（不含扩展名）")
    p_show.add_argument("--map", default="Maps/Game/Stage/LEVEL_Stg111_Map",
                        help="舞台地图资源路径（不含扩展名）")
    p_show.add_argument("--sequence", default="Sequence/Live/Sng001/LS_Sng001_Common_Camera",
                        help="相机 LevelSequence 资源路径（不含扩展名）")
    p_show.add_argument("--binding", default="CamAllA", help="相机绑定名")
    p_show.add_argument("--character-binding", default="CamCharaA",
                        help="角色跟拍相机绑定名（用它的瞄准点作为角色站位）")
    p_show.add_argument("--anim-fps", type=float, default=60.0, help="动画原始帧率")
    p_show.add_argument("--anim-step", type=int, default=2, help="动画采样步长（2 = 30fps）")
    p_show.add_argument("--camera-fps", type=float, default=30.0, help="相机采样帧率")
    p_show.add_argument("--character-z", type=float, default=0.0,
                        help="角色脚底的世界 Z（舞台主地面）")
    p_show.set_defaults(func=cmd_live_show)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
