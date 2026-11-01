"""基于 moderngl 的最小预览渲染器（验证用，非最终引擎）。"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .ue4.staticmesh import StaticMesh

VERTEX_SHADER = """
    #version 330
    uniform mat4 mvp;
    uniform mat4 model;
    in vec3 in_pos;
    in vec3 in_nrm;
    in vec2 in_uv;
    out vec3 v_nrm;
    out vec3 v_pos;
    out vec2 v_uv;
    void main() {
        v_nrm = mat3(model) * in_nrm;
        v_pos = (model * vec4(in_pos, 1.0)).xyz;
        v_uv = in_uv;
        gl_Position = mvp * vec4(in_pos, 1.0);
    }
"""

FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D base_tex;
    uniform bool has_tex;
    uniform vec3 tint;
    uniform float exposure;
    uniform vec3 shadow_tint;
    uniform float shadow_threshold;
    uniform vec3 blend;
    uniform vec3 add;
    in vec3 v_nrm;
    in vec3 v_pos;
    in vec2 v_uv;
    out vec4 color;
    vec3 to_linear(vec3 c) { return pow(c, vec3(2.2)); }
    vec3 to_srgb(vec3 c) { return pow(c, vec3(1.0 / 2.2)); }
    void main() {
        vec3 n = normalize(v_nrm);
        vec3 L = normalize(vec3(0.35, -0.55, 0.75));
        // 卡通明暗：N·L 归一化后按阈值硬阶跃（布料/身体 0.47，头发 0.94）
        float shade = clamp(dot(n, L) * 0.5 + 0.5, 0.0, 1.0);
        float lit = step(shadow_threshold, shade);
        // 阴影区用色表的粉/米色替换，受光区为白色
        vec3 tone = mix(shadow_tint, vec3(1.0), lit);
        vec3 base = has_tex ? to_linear(texture(base_tex, v_uv).rgb) : to_linear(tint);
        // 材质实例的 BlendColor / AddColor 是配色旋钮（部分服装靠它改色）
        base = base * to_linear(blend) + to_linear(add);
        vec3 c = base * to_linear(tone) * exposure;
        color = vec4(to_srgb(clamp(c, 0.0, 1.0)), 1.0);
    }
"""


def _perspective(fov_deg: float, aspect: float, near: float, far: float) -> np.ndarray:
    f = 1.0 / np.tan(np.radians(fov_deg) / 2)
    return np.array(
        [[f / aspect, 0, 0, 0],
         [0, f, 0, 0],
         [0, 0, (far + near) / (near - far), (2 * far * near) / (near - far)],
         [0, 0, -1, 0]], dtype=np.float32)


def _look_at(eye: np.ndarray, target: np.ndarray, up: np.ndarray) -> np.ndarray:
    forward = target - eye
    forward = forward / np.linalg.norm(forward)
    up = up / np.linalg.norm(up)
    right = np.cross(forward, up)
    right = right / np.linalg.norm(right)
    true_up = np.cross(right, forward)
    m = np.eye(4, dtype=np.float32)
    m[0, :3] = right
    m[1, :3] = true_up
    m[2, :3] = -forward
    m[0, 3] = -np.dot(right, eye)
    m[1, 3] = -np.dot(true_up, eye)
    m[2, 3] = np.dot(forward, eye)
    return m


def _smooth_normals(positions: np.ndarray, indices: np.ndarray) -> np.ndarray:
    normals = np.zeros_like(positions)
    tri = indices.reshape(-1, 3).astype(np.int64)
    a, b, c = positions[tri[:, 0]], positions[tri[:, 1]], positions[tri[:, 2]]
    face = np.cross(b - a, c - a)
    for i in range(3):
        np.add.at(normals, tri[:, i], face)
    length = np.linalg.norm(normals, axis=1, keepdims=True)
    length[length == 0] = 1
    return (normals / length).astype(np.float32)


@dataclass
class SceneItem:
    positions: np.ndarray
    indices: np.ndarray
    uvs: np.ndarray | None = None
    texture: np.ndarray | None = None
    model: np.ndarray | None = None
    shadow_tint: tuple[float, float, float] = (1.0, 1.0, 1.0)
    threshold: float = 0.47
    blend: tuple[float, float, float] = (1.0, 1.0, 1.0)
    add: tuple[float, float, float] = (0.0, 0.0, 0.0)


def _upload_texture(ctx, array: np.ndarray):
    import moderngl

    height, width = array.shape[:2]
    rgba = np.ascontiguousarray(array[:, :, :4] if array.shape[2] == 4
                                else np.dstack([array, np.ones(array.shape[:2], np.uint8)]))
    texture = ctx.texture((width, height), 4, rgba.astype("u1").tobytes())
    if max(width, height) >= 256:
        texture.build_mipmaps()
        texture.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
    else:
        texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
    return texture


def render_scene(items: list[SceneItem], out_path: str, size: int = 1024,
                 eye: np.ndarray | None = None, target: np.ndarray | None = None,
                 exposure: float = 1.0, fov: float = 45.0,
                 tint: tuple[float, float, float] = (0.78, 0.76, 0.72)) -> None:
    import moderngl
    from PIL import Image

    lo = np.min([item.positions.min(axis=0) for item in items], axis=0)
    hi = np.max([item.positions.max(axis=0) for item in items], axis=0)
    center = (lo + hi) * 0.5
    radius = float(np.linalg.norm(hi - lo) * 0.5) or 1.0
    if eye is None:
        eye = center + np.array([0.0, radius * 1.35, radius * 0.18], dtype=np.float32)
    if target is None:
        target = center
    eye = np.asarray(eye, dtype=np.float32)
    target = np.asarray(target, dtype=np.float32)

    ctx = moderngl.create_standalone_context(require=330)
    fbo = ctx.simple_framebuffer((size, size))
    fbo.use()
    ctx.clear(0.05, 0.06, 0.08, 1.0)
    ctx.enable(moderngl.DEPTH_TEST)

    program = ctx.program(vertex_shader=VERTEX_SHADER, fragment_shader=FRAGMENT_SHADER)
    view_proj = _perspective(fov, 1.0, max(radius * 0.002, 0.05), radius * 30 + 100) @ _look_at(
        eye, target, np.array([0.0, 0.0, 1.0], dtype=np.float32))
    program["tint"].value = tint
    program["exposure"].value = float(exposure)

    for item in items:
        positions = item.positions
        normals = _smooth_normals(positions, item.indices)
        uv_array = item.uvs if item.uvs is not None else np.zeros((len(positions), 2), np.float32)
        model = item.model if item.model is not None else np.eye(4, dtype=np.float32)

        vbo = ctx.buffer(np.hstack([positions, normals, uv_array]).astype(np.float32).tobytes())
        ibo = ctx.buffer(item.indices.astype(np.uint16).tobytes() if len(positions) < 65535
                         else item.indices.astype(np.uint32).tobytes())
        vao = ctx.vertex_array(
            program, [(vbo, "3f 3f 2f", "in_pos", "in_nrm", "in_uv")], ibo,
            index_element_size=2 if len(positions) < 65535 else 4)
        mvp = view_proj @ model
        program["mvp"].write(np.ascontiguousarray(mvp.T).astype(np.float32).tobytes())
        program["model"].write(np.ascontiguousarray(model.T).astype(np.float32).tobytes())
        program["shadow_tint"].value = tuple(item.shadow_tint)
        program["shadow_threshold"].value = float(item.threshold)
        program["blend"].value = tuple(item.blend)
        program["add"].value = tuple(item.add)
        if item.texture is not None:
            texture = _upload_texture(ctx, item.texture)
            texture.use(0)
            program["base_tex"].value = 0
            program["has_tex"].value = True
        else:
            program["has_tex"].value = False
        vao.render()

    pixels = fbo.read(components=3)
    image = Image.frombytes("RGB", fbo.size, pixels).transpose(Image.FLIP_TOP_BOTTOM)
    image.save(out_path)


def render_static_mesh(mesh: StaticMesh, out_path: str, size: int = 1024,
                       base_color: np.ndarray | None = None,
                       tint: tuple[float, float, float] = (0.78, 0.76, 0.72),
                       exposure: float = 1.0) -> None:
    positions = mesh.positions
    center = (positions.min(axis=0) + positions.max(axis=0)) * 0.5
    radius = float(np.linalg.norm(positions.max(axis=0) - positions.min(axis=0)) * 0.5) or 1.0
    eye = center + np.array([radius * 2.2, -radius * 1.6, radius * 1.2], dtype=np.float32)
    item = SceneItem(positions, mesh.indices, mesh.uvs, base_color)
    render_scene([item], out_path, size, eye, center, exposure,
                 tint=(1.0, 1.0, 1.0) if base_color is not None else tint)
