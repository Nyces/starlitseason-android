"""sspak 命令行：pak 校验、资产普查、条目列举与提取。"""

from __future__ import annotations

import argparse
import collections
import os
import sys

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
        except Exception as exc:  # noqa: BLE001
            print(f"  [warn] 材质解析失败 {material}: {exc}")
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


def cmd_preview(args) -> int:
    paks = _open_all(args)
    try:
        from .ue4.content import ContentStore
        from .ue4.staticmesh import parse_static_mesh
        from .render import render_static_mesh

        store = ContentStore(paks)
        package = store.package(args.asset)
        if package is None:
            raise SystemExit(f"未找到资源: {args.asset}")
        export = package.find_export("StaticMesh")
        if export is None:
            raise SystemExit("该资源不包含 StaticMesh")
        mesh = parse_static_mesh(package.export_bytes(export), package)

        base_color = None
        if not args.no_texture:
            material = args.material or (mesh.material_slots[0] if mesh.material_slots else None)
            param, base_color = _material_texture(store, material)
            if param:
                print(f"  材质 {material}\n    参数 {param}")
            else:
                print("  [warn] 未解析到基础色贴图，使用纯色着色")

        exposure = 1.0
        if base_color is not None:
            import numpy as np
            mean = float(base_color[:, :, :3].astype("f4").mean() / 255.0)
            exposure = float(np.clip(0.30 / max(mean ** 2.2, 1e-4), 1.0, 6.0))
            print(f"  贴图 {base_color.shape[1]}x{base_color.shape[0]}，平均色 {mean:.3f}，"
                  f"预览曝光 ×{exposure:.1f}")

        render_static_mesh(mesh, args.out, args.size, base_color, exposure=exposure)
        print(f"已渲染 {args.asset} -> {args.out} "
              f"({len(mesh.positions)} 顶点 / {len(mesh.indices) // 3} 三角形"
              f"{'，带贴图' if base_color is not None else ''})")
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


def cmd_scene(args) -> int:
    paks = _open_all(args)
    try:
        import numpy as np

        from .ue4.content import ContentStore
        from .ue4.level import parse_level
        from .ue4.staticmesh import parse_static_mesh
        from .render import SceneItem, render_scene

        store = ContentStore(paks)
        package = store.package(args.map)
        if package is None:
            raise SystemExit(f"未找到地图: {args.map}")
        placed = parse_level(package)
        print(f"关卡 {args.map}：静态网格 actor {len(placed)} 个")

        mesh_cache: dict[str, object] = {}
        material_cache: dict[str, object] = {}
        items = []
        skipped = 0
        for index, entry in enumerate(placed):
            if args.limit and len(items) >= args.limit:
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
                        print(f"  [warn] 网格解析失败 {entry.mesh}: {exc}")
                        mesh_cache[entry.mesh] = None
            mesh = mesh_cache[entry.mesh]
            if mesh is None:
                skipped += 1
                continue

            texture = None
            if not args.no_texture:
                material = (entry.materials[0] if entry.materials and entry.materials[0]
                            else (mesh.material_slots[0] if mesh.material_slots else None))
                if material and material not in material_cache:
                    material_cache[material] = _material_texture(store, material)[1]
                texture = material_cache.get(material) if material else None

            items.append(SceneItem(mesh.positions, mesh.indices, mesh.uvs, texture,
                                   _placement_matrix(entry.location, entry.rotation, entry.scale)))
            if (index + 1) % 20 == 0:
                print(f"  ... 已装载 {index + 1}/{len(placed)}")

        if not items:
            raise SystemExit("没有可渲染的静态网格")

        exposure = 1.0
        if not args.no_texture:
            means = [item.texture[:, :, :3].astype("f4").mean() / 255.0
                     for item in items if item.texture is not None]
            if means:
                mean = float(np.mean(means))
                exposure = float(np.clip(0.30 / max(mean ** 2.2, 1e-4), 1.0, 6.0))
                print(f"  {len(means)} 张贴图，平均色 {mean:.3f}，预览曝光 ×{exposure:.1f}")

        eye = target = None
        if args.camera:
            values = [float(v) for v in args.camera.split(",")]
            if len(values) != 6:
                raise SystemExit("--camera 需要 6 个数字: ex,ey,ez,tx,ty,tz")
            eye = np.array(values[:3], dtype=np.float32)
            target = np.array(values[3:], dtype=np.float32)

        render_scene(items, args.out, args.size, eye, target, exposure, args.fov)
        print(f"已渲染 {len(items)} 个网格 -> {args.out}（跳过 {skipped} 个）")
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
    p_preview.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_preview.set_defaults(func=cmd_preview)

    p_scene = sub.add_parser("scene", help="渲染关卡中的静态网格组合画面")
    p_scene.add_argument("--map", required=True, help="地图资源路径（不含扩展名）")
    p_scene.add_argument("--out", required=True, help="输出 PNG 路径")
    p_scene.add_argument("--size", type=int, default=1024)
    p_scene.add_argument("--fov", type=float, default=45.0)
    p_scene.add_argument("--camera", help="ex,ey,ez,tx,ty,tz")
    p_scene.add_argument("--limit", type=int, default=0, help="最多装载多少个网格")
    p_scene.add_argument("--no-texture", action="store_true", help="只渲染纯色")
    p_scene.set_defaults(func=cmd_scene)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
