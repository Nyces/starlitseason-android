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
        if result.sphere is None and instance.textures.get("SphereMap"):
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
                     light=_vec3(args.light, (0.0, 0.0, 1.0)))
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
    """主骨架（SK_chr_all_bone）的骨骼名与绑定旋转矩阵。"""
    from .ue4.skeletalmesh import _quat_matrix, parse_skeletal_mesh

    package = store.package("Model/Character/Body/Base/chr_body_base/Mesh/SK_chr_all_bone")
    if package is None:
        raise SystemExit("未找到主骨架网格 SK_chr_all_bone")
    mesh = parse_skeletal_mesh(package)
    names = [name for name, _ in mesh.bones]
    binds = [_quat_matrix(pose[:4]) for pose in mesh.bind_pose]
    return names, binds


def _rotation_angle(matrix) -> float:
    import numpy as np

    return float(np.degrees(np.arccos(np.clip((np.trace(matrix) - 1) / 2, -1.0, 1.0))))


def _anim_pose(store, anim: str, frame: int) -> dict:
    """把动画某一帧解成 {骨骼名: 3×3 局部旋转}（实验性，失败会抛出）。

    键有两种可能语义：绝对局部旋转，或相对绑定姿势的增量。逐轨选与绑定姿势
    偏差更小的解释——待机类动画不会把骨头拧出大角度，这比全局二选一更稳。
    """
    from .ue4.anim import AnimSequenceError, parse_animation, quat_matrix

    package = store.package(anim)
    if package is None:
        raise SystemExit(f"未找到动画: {anim}")
    try:
        sequence = parse_animation(package)
    except AnimSequenceError as exc:
        raise SystemExit(f"动画解析失败 {anim}: {exc}") from exc
    names, binds = _master_bones(store)
    pose = {}
    relative = 0
    for bone, xyz in sequence.sample(frame).items():
        if not 0 <= bone < len(names):
            continue
        rotation = quat_matrix(xyz)
        if _rotation_angle(rotation) < _rotation_angle(rotation.T @ binds[bone]) \
                and _rotation_angle(rotation) > 1.0:
            pose[names[bone]] = rotation @ binds[bone]
            relative += 1
        else:
            pose[names[bone]] = rotation
    print(f"  动画 {anim.rsplit('/', 1)[-1]}：{len(sequence.bone_indices)} 轨 / "
          f"{sequence.num_frames} 帧 / 自检 {sequence.quality:.2f} / 增量解释 {relative} 轨")
    return pose


def _skeletal_items(store, asset: str, no_texture: bool, flip: str = "", pose=None,
                    replace: bool = False):
    from .ue4.skeletalmesh import parse_skeletal_mesh
    from .render import SceneItem

    package = store.package(asset)
    if package is None:
        return [], None, None
    mesh = parse_skeletal_mesh(package)
    positions = mesh.positions
    if pose:
        try:
            positions = mesh.posed_positions(pose, replace=replace)
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
        if slot and "outline" in slot.lower():
            toon.outline = True               # 描边是网格里烘焙的反向壳，剔除正面再画
        items.append(toon.item(positions, indices, _flip_uv(mesh.uvs, flip)))
    return items, mesh, positions


def _head_attachment(mesh, pose, replace: bool):
    """头部骨骼的刚性变换（G_pose · G_bind⁻¹），供没有头部骨骼的头发/饰品跟随。"""
    import numpy as np

    names = [b[0] for b in mesh.bones]
    for bone in ("SC_Head", "AO_Head"):
        if bone in names:
            i = names.index(bone)
            posed = mesh.posed_globals(pose, replace)
            bind = mesh.bind_matrices()
            return posed[i] @ np.linalg.inv(bind[i])
    return np.eye(4)


def _character_items(store, chr_id: str, no_texture: bool, flip: str = "", verbose: bool = True,
                     pose=None, replace: bool = False):
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
    head_bind_center = None
    head_matrix = np.eye(4, dtype=np.float64)
    for asset in candidates:
        # 头发/饰品的骨骼里没有头部骨骼，不能直接吃身体动画，改用头部刚性变换跟随
        attached = asset.endswith(("_hair", "_orna"))
        mesh_items, mesh, posed = _skeletal_items(
            store, asset, no_texture, flip, None if attached else pose, replace)
        if not mesh_items:
            continue
        if verbose:
            print(f"  {asset.rsplit('/', 1)[-1]}: {len(mesh.positions)} 顶点 / "
                  f"{len(mesh.sections)} 分段 / {len(mesh.bones)} 骨骼")
        bind_center = (mesh.positions.min(axis=0) + mesh.positions.max(axis=0)) * 0.5
        if asset.endswith("_face"):
            head_bind_center = bind_center
        elif not attached and pose:
            head_matrix = _head_attachment(mesh, pose, replace)
        if attached and head_bind_center is not None:
            shift = np.eye(4, dtype=np.float64)
            shift[:3, 3] = head_bind_center - bind_center
            model = head_matrix @ shift
            for item in mesh_items:
                item.model = model.astype(np.float32)
            items.extend(mesh_items)
            world = posed @ model[:3, :3].T + model[:3, 3]
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
        if args.anim:
            # 实验性：_anim_pose 已把绑定旋转合进矩阵，这里直接替换局部旋转
            pose = _anim_pose(store, args.anim, args.frame)
            replace = True
        items, center, radius = _character_items(store, chr_id, args.no_texture, args.flip_uv,
                                                 pose=pose, replace=replace)
        if not items:
            raise SystemExit("没有可渲染的骨骼网格")

        if args.camera:
            values = [float(v) for v in args.camera.split(",")]
            eye, target = np.array(values[:3], np.float32), np.array(values[3:], np.float32)
        else:
            target = center
            eye = center + np.array([0.0, -radius * 2.2, radius * 0.12], dtype=np.float32)

        render_scene(items, args.out, args.size, eye, target, args.exposure, args.fov,
                     light=_vec3(args.light, (0.0, 0.0, 1.0)))
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
            if args.anim:
                pose = _anim_pose(store, args.anim, args.frame)
                replace = True
            char_items, character_center, character_radius = _character_items(
                store, args.character, args.no_texture, args.flip_uv, verbose=False,
                pose=pose, replace=replace)
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
    p_scene.add_argument("--anim", help="实验性：把角色摆成 AnimSequence 的某一帧（只解旋转通道）")
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
                             help="实验性：用 AnimSequence 的某一帧摆姿势（只解旋转通道）")
    p_character.add_argument("--frame", type=int, default=0, help="--anim 的帧号")
    p_character.add_argument("--flip-uv", default="", choices=["", "none", "u", "v", "uv"],
                             help="翻转 UV 轴，用于排查贴图方向")
    p_character.set_defaults(func=cmd_character)

    p_shaders = sub.add_parser("shaders", help="导出材质里的 SM5 着色器反汇编")
    p_shaders.add_argument("--asset", required=True, help="材质路径（不含扩展名）")
    p_shaders.add_argument("--out", required=True, help="输出 TXT 路径")
    p_shaders.add_argument("--max", type=int, default=0, help="最多导出多少个着色器")
    p_shaders.set_defaults(func=cmd_shaders)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
