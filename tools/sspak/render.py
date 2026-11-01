"""基于 moderngl 的最小预览渲染器（验证用，非最终引擎）。"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .ue4.staticmesh import StaticMesh

VERTEX_SHADER = """
    #version 330
    uniform mat4 mvp;
    uniform mat4 model;
    uniform mat4 view;
    in vec3 in_pos;
    in vec3 in_nrm;
    in vec2 in_uv;
    out vec3 v_nrm;
    out vec3 v_nrm_view;
    out vec3 v_pos;
    out vec2 v_uv;
    void main() {
        vec3 n = mat3(model) * in_nrm;
        float len = length(n);
        v_nrm = len > 1e-5 ? n / len : vec3(0.0, 0.0, 1.0);
        v_nrm_view = mat3(view) * v_nrm;
        v_pos = (model * vec4(in_pos, 1.0)).xyz;
        v_uv = in_uv;
        gl_Position = mvp * vec4(in_pos, 1.0);
    }
"""

FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D base_tex;
    uniform sampler2D sphere_tex;
    uniform sampler2D emissive_tex;
    uniform sampler2D shadow_tex;
    uniform bool has_tex;
    uniform bool has_sphere;
    uniform bool has_emissive;
    uniform bool has_shadow;
    uniform vec3 tint;
    uniform vec3 blend;
    uniform vec3 add;
    uniform vec3 shadow_tint;
    uniform float shadow_threshold;
    uniform float shadow_intensity;
    uniform float lighting_intensity;
    uniform vec3 light_dir;
    uniform vec3 eye;
    uniform vec3 sphere_blend;
    uniform float sphere_pow;
    uniform float sphere_boost;
    uniform vec3 rim_color;
    uniform float rim_scale;
    uniform float rim_intensity;
    uniform float emissive_boost;
    uniform float exposure;
    in vec3 v_nrm;
    in vec3 v_nrm_view;
    in vec3 v_pos;
    in vec2 v_uv;
    out vec4 color;
    vec3 to_linear(vec3 c) { return pow(c, vec3(2.2)); }
    vec3 to_srgb(vec3 c) { return pow(c, vec3(1.0 / 2.2)); }
    void main() {
        vec3 n = normalize(v_nrm);
        vec3 v = normalize(eye - v_pos);
        // 明暗：ToonShaderParameter 的 LightVector（世界空间）与 ToonShadowIntensity
        float shade = dot(n, normalize(light_dir)) * 0.5 + 0.5;
        shade = clamp((shade - 0.5) * shadow_intensity + 0.5, 0.0, 1.0);
        // ShadowTexture 的 alpha 标记哪里允许出现卡通阴影（脸/身体靠它收窄阴影）
        float mask = has_shadow ? texture(shadow_tex, v_uv).a : 1.0;
        float lit = step(shadow_threshold, mix(1.0, shade, mask));
        // 阴影区用色表的粉/米色替换，受光区为白色
        vec3 tone = mix(shadow_tint, vec3(1.0), lit);
        vec3 base = has_tex ? to_linear(texture(base_tex, v_uv).rgb) : to_linear(tint);
        // 材质实例的 BlendColor / AddColor 是配色旋钮（部分服装靠它改色）
        base = base * to_linear(blend) + to_linear(add);
        // MatCap（SphereMap）：视空间法线 xy 映射到球面贴图，先乘 BlendColor 再取幂、乘 Boost
        if (has_sphere) {
            vec3 nv = normalize(v_nrm_view);
            vec2 uv = nv.xy * vec2(0.5, -0.5) + 0.5;
            vec3 s = max(texture(sphere_tex, uv).rgb * sphere_blend, vec3(0.0));
            s = pow(s, vec3(sphere_pow)) * sphere_boost;
            base += to_linear(s);
        }
        vec3 c = base * to_linear(tone) * lighting_intensity;
        // 边缘光：pow(1-|N·V|, RimScale)（与 ToonBaseCloth 反汇编一致）
        float rim = pow(clamp(1.0 - abs(dot(n, v)), 0.0, 1.0), rim_scale) * rim_intensity;
        c += to_linear(rim_color) * rim;
        if (has_emissive) {
            c += to_linear(texture(emissive_tex, v_uv).rgb) * emissive_boost;
        }
        color = vec4(to_srgb(clamp(c * exposure, 0.0, 1.0)), 1.0);
    }
"""

# 描边：网格里已经烘焙了外扩壳（材质槽 *_outline），这里沿法线再外扩一点并剔除正面，
# 对应 ToonOutline 里 OutlineScale / DepthOffsetScale 的效果。
OUTLINE_VERTEX_SHADER = """
    #version 330
    uniform mat4 view_proj;
    uniform mat4 model;
    uniform vec3 eye;
    uniform float outline_width;
    in vec3 in_pos;
    in vec3 in_nrm;
    void main() {
        vec3 n = mat3(model) * in_nrm;
        float len = length(n);
        vec4 world = model * vec4(in_pos, 1.0);
        if (len > 1e-5) {
            world.xyz += (n / len) * outline_width * length(eye - world.xyz);
        }
        gl_Position = view_proj * world;
    }
"""

OUTLINE_FRAGMENT_SHADER = """
    #version 330
    uniform vec3 line_color;
    out vec4 color;
    void main() {
        color = vec4(line_color, 1.0);
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
    sphere: np.ndarray | None = None
    sphere_blend: tuple[float, float, float] = (1.0, 1.0, 1.0)
    sphere_pow: float = 1.0
    sphere_boost: float = 1.0
    shadow_map: np.ndarray | None = None
    rim_scale: float = 7.5
    rim_intensity: float = 1.0
    emissive: np.ndarray | None = None
    emissive_boost: float = 1.0
    outline: bool = False
    outline_color: tuple[float, float, float] = (0.16, 0.10, 0.09)
    outline_width: float = 0.0008


def _set(program, name: str, value) -> None:
    """设置着色器里存在的 uniform（不同材质用到的参数不同）。"""
    if name not in program:
        return
    program[name].value = value


def _set_matrix(program, name: str, matrix: np.ndarray) -> None:
    if name not in program:
        return
    program[name].write(np.ascontiguousarray(matrix.T).astype(np.float32).tobytes())


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
                 tint: tuple[float, float, float] = (0.78, 0.76, 0.72),
                 light: tuple[float, float, float] = (0.0, 0.0, 1.0),
                 shadow_intensity: float = 1.5,
                 lighting_intensity: float = 1.0) -> None:
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
    outline_program = ctx.program(vertex_shader=OUTLINE_VERTEX_SHADER,
                                  fragment_shader=OUTLINE_FRAGMENT_SHADER)
    view = _look_at(eye, target, np.array([0.0, 0.0, 1.0], dtype=np.float32))
    view_proj = _perspective(fov, 1.0, max(radius * 0.002, 0.05), radius * 30 + 100) @ view
    _set(program, "tint", tint)
    _set(program, "exposure", float(exposure))
    _set(program, "eye", tuple(float(v) for v in eye))
    _set(program, "light_dir", light)
    _set(program, "shadow_intensity", float(shadow_intensity))
    _set(program, "lighting_intensity", float(lighting_intensity))
    _set(program, "rim_color", (1.0, 1.0, 1.0))
    _set(program, "rim_intensity", 1.0)
    _set_matrix(program, "view", view)

    white = ctx.texture((1, 1), 4, bytes((255, 255, 255, 255)))
    white.use(1)
    _set(program, "sphere_tex", 1)
    white.use(2)
    _set(program, "emissive_tex", 2)
    white.use(3)
    _set(program, "shadow_tex", 3)

    prepared = []
    for item in items:
        positions = item.positions
        normals = _smooth_normals(positions, item.indices)
        uv_array = item.uvs if item.uvs is not None else np.zeros((len(positions), 2), np.float32)
        model = item.model if item.model is not None else np.eye(4, dtype=np.float32)
        vbo = ctx.buffer(np.hstack([positions, normals, uv_array]).astype(np.float32).tobytes())
        small = len(positions) < 65535
        ibo = ctx.buffer(item.indices.astype(np.uint16).tobytes() if small
                         else item.indices.astype(np.uint32).tobytes())
        prepared.append((item, normals, model, vbo, ibo, small, view_proj @ model))

    # 描边先画：网格里的外扩壳沿法线外推，剔除正面只留轮廓
    outlines = [entry for entry in prepared if entry[0].outline]
    if outlines:
        ctx.enable(moderngl.CULL_FACE)
        ctx.cull_face = "front"
        for item, normals, model, vbo, ibo, small, _ in outlines:
            # 描边只用位置+法线，必须单独建缓冲（否则步长与主 pass 的 8 float 不符）
            outline_vbo = ctx.buffer(
                np.hstack([item.positions, normals]).astype(np.float32).tobytes())
            vao = ctx.vertex_array(
                outline_program, [(outline_vbo, "3f 3f", "in_pos", "in_nrm")], ibo,
                index_element_size=2 if small else 4)
            _set_matrix(outline_program, "view_proj", view_proj)
            _set_matrix(outline_program, "model", model)
            _set(outline_program, "eye", tuple(float(v) for v in eye))
            _set(outline_program, "outline_width", float(item.outline_width))
            _set(outline_program, "line_color", tuple(item.outline_color))
            vao.render()
        ctx.disable(moderngl.CULL_FACE)

    for item, normals, model, vbo, ibo, small, mvp in prepared:
        if item.outline:
            continue
        vao = ctx.vertex_array(
            program, [(vbo, "3f 3f 2f", "in_pos", "in_nrm", "in_uv")], ibo,
            index_element_size=2 if small else 4)
        _set_matrix(program, "mvp", mvp)
        _set_matrix(program, "model", model)
        _set(program, "shadow_tint", tuple(item.shadow_tint))
        _set(program, "shadow_threshold", float(item.threshold))
        _set(program, "blend", tuple(item.blend))
        _set(program, "add", tuple(item.add))
        _set(program, "rim_scale", float(item.rim_scale))
        _set(program, "rim_intensity", float(item.rim_intensity))
        if item.texture is not None:
            texture = _upload_texture(ctx, item.texture)
            texture.use(0)
            _set(program, "base_tex", 0)
            _set(program, "has_tex", True)
        else:
            _set(program, "has_tex", False)
        if item.sphere is not None:
            sphere = _upload_texture(ctx, item.sphere)
            sphere.use(1)
            _set(program, "has_sphere", True)
            _set(program, "sphere_blend", tuple(item.sphere_blend))
            _set(program, "sphere_pow", float(item.sphere_pow))
            _set(program, "sphere_boost", float(item.sphere_boost))
        else:
            _set(program, "has_sphere", False)
        if item.emissive is not None:
            emissive = _upload_texture(ctx, item.emissive)
            emissive.use(2)
            _set(program, "has_emissive", True)
            _set(program, "emissive_boost", float(item.emissive_boost))
        else:
            _set(program, "has_emissive", False)
        if item.shadow_map is not None:
            shadow_map = _upload_texture(ctx, item.shadow_map)
            shadow_map.use(3)
            _set(program, "has_shadow", True)
        else:
            _set(program, "has_shadow", False)
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
