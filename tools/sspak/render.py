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
    uniform sampler2D shadow_map;
    uniform mat4 light_vp;
    uniform bool has_shadowmap;
    uniform float shadow_strength;
    uniform bool unlit;
    uniform float alpha_cut;
    uniform bool use_screen;
    uniform vec3 screen_lo;
    uniform vec3 screen_hi;
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
        float alpha = 1.0;
        vec3 base;
        if (has_tex) {
            vec4 texel = texture(base_tex, v_uv);
            alpha = texel.a;
            base = to_linear(texel.rgb);
        } else {
            base = to_linear(tint);
        }
        if (alpha_cut > 0.0 && alpha < alpha_cut) {
            discard;                       // BLEND_Masked 的抠像
        }
        // 材质实例的 BlendColor / AddColor 是配色旋钮（部分服装靠它改色）
        base = base * to_linear(blend) + to_linear(add);
        if (!unlit) {
            // 明暗：ToonShaderParameter 的 LightVector（世界空间）与 ToonShadowIntensity
            float shade = dot(n, normalize(light_dir)) * 0.5 + 0.5;
            shade = clamp((shade - 0.5) * shadow_intensity + 0.5, 0.0, 1.0);
            // ShadowTexture 的 alpha 标记哪里允许出现卡通阴影（脸/身体靠它收窄阴影）
            float mask = has_shadow ? texture(shadow_tex, v_uv).a : 1.0;
            float lit = step(shadow_threshold, mix(1.0, shade, mask));
            float shadow_amount = 0.0;
            // 方向光阴影贴图（3x3 PCF）
            if (has_shadowmap) {
                // 沿法线偏移采样点，压掉自阴影粉刺
                vec4 lp = light_vp * vec4(v_pos + n * 2.0, 1.0);
                vec3 proj = lp.xyz / lp.w;
                if (abs(proj.x) <= 1.0 && abs(proj.y) <= 1.0 && abs(proj.z) <= 1.0) {
                    vec2 uv = proj.xy * 0.5 + 0.5;
                    float depth = proj.z * 0.5 + 0.5;
                    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0));
                    float sum = 0.0;
                    for (int y = -1; y <= 1; ++y) {
                        for (int x = -1; x <= 1; ++x) {
                            float stored = texture(shadow_map, uv + vec2(x, y) * texel).r;
                            sum += (depth - 0.004) <= stored ? 1.0 : 0.0;
                        }
                    }
                    shadow_amount = (1.0 - sum / 9.0) * shadow_strength;
                    lit *= 1.0 - shadow_amount;
                }
            }
            // 阴影区用色表的粉/米色替换，受光区为白色
            vec3 tone = mix(shadow_tint, vec3(1.0), lit);
            // MatCap（SphereMap）：视空间法线 xy 映射到球面贴图，先乘 BlendColor 再取幂、乘 Boost
            if (has_sphere) {
                vec3 nv = normalize(v_nrm_view);
                vec2 uv = nv.xy * vec2(0.5, -0.5) + 0.5;
                vec3 s = max(texture(sphere_tex, uv).rgb * sphere_blend, vec3(0.0));
                s = pow(s, vec3(sphere_pow)) * sphere_boost;
                base += to_linear(s);
            }
            base = base * to_linear(tone) * lighting_intensity;
            // 没有卡通色表的材质（舞台/道具）靠这一项才能看出投影
            base *= mix(vec3(1.0), vec3(0.60, 0.61, 0.68), shadow_amount);
            // 边缘光：pow(1-|N·V|, RimScale)（与 ToonBaseCloth 反汇编一致）
            float rim = pow(clamp(1.0 - abs(dot(n, v)), 0.0, 1.0), rim_scale) * rim_intensity;
            base += to_linear(rim_color) * rim;
            if (has_emissive) {
                base += to_linear(texture(emissive_tex, v_uv).rgb) * emissive_boost;
            }
        } else if (has_emissive) {
            // 无光照的发光材质：只用发光项（Emissive_Additive 系列）
            base = to_linear(texture(emissive_tex, v_uv).rgb) * emissive_boost;
        }
        // 心形大屏等运行时画面：游戏里由渲染目标覆盖，离线预览用中性渐变占位
        if (use_screen) {
            vec3 span = max(screen_hi - screen_lo, vec3(1e-4));
            vec3 t = clamp((v_pos - screen_lo) / span, 0.0, 1.0);
            vec3 top = vec3(0.99, 0.97, 0.96);
            vec3 bottom = vec3(0.45, 0.40, 0.45);
            float vignette = 1.0 - 0.22 * pow(abs(t.x - 0.5) * 2.0, 2.0);
            base = to_linear(mix(bottom, top, t.z) * vignette);
            alpha = 1.0;
        }
        // 输出线性 HDR，曝光与 sRGB 编码交给后处理
        color = vec4(base, alpha);
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


POST_VERTEX_SHADER = """
    #version 330
    in vec2 in_pos;
    out vec2 v_uv;
    void main() {
        v_uv = in_pos * 0.5 + 0.5;
        gl_Position = vec4(in_pos, 0.0, 1.0);
    }
"""

BRIGHT_FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D scene;
    uniform float threshold;
    in vec2 v_uv;
    out vec4 color;
    void main() {
        vec3 c = texture(scene, v_uv).rgb;
        float level = max(c.r, max(c.g, c.b));
        color = vec4(c * max(level - threshold, 0.0) / max(level, 1e-4), 1.0);
    }
"""

BLUR_FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D src;
    uniform vec2 direction;
    in vec2 v_uv;
    out vec4 color;
    void main() {
        vec3 sum = texture(src, v_uv).rgb * 0.227027;
        sum += (texture(src, v_uv + direction * 1.3846).rgb
                + texture(src, v_uv - direction * 1.3846).rgb) * 0.316216;
        sum += (texture(src, v_uv + direction * 3.2308).rgb
                + texture(src, v_uv - direction * 3.2308).rgb) * 0.070270;
        color = vec4(sum, 1.0);
    }
"""

AO_FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D depth_tex;
    uniform float strength;
    in vec2 v_uv;
    out vec4 color;
    const vec2 KERNEL[8] = vec2[8](
        vec2( 0.000, -0.010), vec2(-0.008, -0.006), vec2( 0.008, -0.006),
        vec2(-0.010,  0.000), vec2( 0.010,  0.000), vec2(-0.008,  0.006),
        vec2( 0.008,  0.006), vec2( 0.000,  0.010));
    void main() {
        float d = texture(depth_tex, v_uv).r;
        if (d >= 0.99999) {
            color = vec4(1.0);
            return;
        }
        float occluded = 0.0;
        for (int i = 0; i < 8; ++i) {
            float sd = texture(depth_tex, v_uv + KERNEL[i]).r;
            float diff = d - sd;
            if (diff > 0.0006 && diff < 0.02) {
                occluded += 1.0;
            }
        }
        color = vec4(vec3(1.0 - occluded / 8.0 * strength), 1.0);
    }
"""

COMPOSITE_FRAGMENT_SHADER = """
    #version 330
    uniform sampler2D scene;
    uniform sampler2D bloom;
    uniform sampler2D ao;
    uniform bool has_ao;
    uniform float intensity;
    uniform float exposure;
    in vec2 v_uv;
    out vec4 color;
    vec3 to_srgb(vec3 c) { return pow(c, vec3(1.0 / 2.2)); }
    void main() {
        vec4 scene_color = texture(scene, v_uv);
        vec3 c = scene_color.rgb * exposure;
        if (has_ao) {
            c *= texture(ao, v_uv).r;
        }
        c += texture(bloom, v_uv).rgb * intensity;
        color = vec4(to_srgb(clamp(c, 0.0, 1.0)), 1.0);
    }
"""

SHADOW_VERTEX_SHADER = """
    #version 330
    uniform mat4 light_vp;
    uniform mat4 model;
    in vec3 in_pos;
    void main() {
        gl_Position = light_vp * model * vec4(in_pos, 1.0);
    }
"""

SHADOW_FRAGMENT_SHADER = """
    #version 330
    out vec4 color;
    void main() {
        color = vec4(1.0);
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


def _orthographic(half: float, near: float, far: float) -> np.ndarray:
    return np.array(
        [[1.0 / half, 0, 0, 0],
         [0, 1.0 / half, 0, 0],
         [0, 0, -2.0 / (far - near), -(far + near) / (far - near)],
         [0, 0, 0, 1]], dtype=np.float32)


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
    blend_mode: str = "opaque"          # opaque / masked / additive / translucent / modulate
    unlit: bool = False
    screen: bool = False                # 大屏占位：按世界坐标自上而下渐变
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
                 # 默认主光来自前上方：正上方打光时 ToonBaseFace 的 dot(N,L) 分界线
                 # 正好横落在鼻梁和两颊上，渲出一条横贯脸的色带。
                 light: tuple[float, float, float] = (0.0, 0.55, 0.83),
                 shadow_intensity: float = 1.5,
                 lighting_intensity: float = 1.0,
                 shadow: bool = True, shadow_strength: float = 0.85,
                 shadow_center: np.ndarray | None = None,
                 shadow_radius: float | None = None,
                 bloom: bool = True, bloom_threshold: float = 1.05,
                 bloom_intensity: float = 0.3,
                 ao: bool = True, ao_strength: float = 0.45) -> None:
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
    # 主场景渲到 HDR，曝光/泛光/编码在最后一步做
    scene_texture = ctx.texture((size, size), 4, dtype="f2")
    scene_texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
    scene_depth = ctx.depth_texture((size, size))
    fbo = ctx.framebuffer(color_attachments=[scene_texture], depth_attachment=scene_depth)
    fbo.use()
    ctx.clear(0.01, 0.012, 0.016, 1.0)
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

    shadow_tex = None
    light_vp = np.eye(4, dtype=np.float32)

    def draw(entry) -> None:
        item, normals, model, vbo, ibo, small, mvp = entry
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
        _set(program, "unlit", bool(item.unlit))
        _set(program, "alpha_cut", 0.33 if item.blend_mode == "masked" else 0.0)
        if item.screen:
            lo_local = item.positions.min(axis=0)
            hi_local = item.positions.max(axis=0)
            corners = np.array([[x, y, z, 1.0]
                                for x in (lo_local[0], hi_local[0])
                                for y in (lo_local[1], hi_local[1])
                                for z in (lo_local[2], hi_local[2])], dtype=np.float32)
            world = corners @ model.T
            _set(program, "use_screen", True)
            _set(program, "screen_lo", tuple(world[:, :3].min(axis=0).tolist()))
            _set(program, "screen_hi", tuple(world[:, :3].max(axis=0).tolist()))
        else:
            _set(program, "use_screen", False)
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
        if shadow_tex is not None:
            shadow_tex.use(4)
            _set(program, "shadow_map", 4)
            _set_matrix(program, "light_vp", light_vp)
            _set(program, "has_shadowmap", True)
            _set(program, "shadow_strength", float(shadow_strength))
        else:
            _set(program, "has_shadowmap", False)
        vao.render()

    transparent_modes = ("additive", "translucent", "modulate")
    solid = [entry for entry in prepared
             if not entry[0].outline and entry[0].blend_mode not in transparent_modes]
    blended = [entry for entry in prepared
               if not entry[0].outline and entry[0].blend_mode in transparent_modes]

    if shadow and solid:
        # 从光源方向渲一张深度图，正交投影覆盖指定范围（默认整个场景）
        light_dir = np.asarray(light, dtype=np.float32)
        norm = float(np.linalg.norm(light_dir))
        light_dir = light_dir / norm if norm > 1e-6 else np.array([0.0, 0.0, 1.0], np.float32)
        up = (np.array([0.0, 0.0, 1.0], np.float32) if abs(light_dir[2]) < 0.9
              else np.array([0.0, 1.0, 0.0], np.float32))
        focus = center if shadow_center is None else np.asarray(shadow_center, np.float32)
        span = radius if shadow_radius is None else float(shadow_radius)
        light_view = _look_at(focus + light_dir * span * 4.0, focus, up)
        light_vp = _orthographic(span * 1.6, 0.1, span * 8.0 + 100.0) @ light_view

        shadow_tex = ctx.depth_texture((2048, 2048))
        shadow_tex.filter = (moderngl.NEAREST, moderngl.NEAREST)
        shadow_fbo = ctx.framebuffer(depth_attachment=shadow_tex)
        shadow_fbo.use()
        shadow_fbo.clear(depth=1.0)
        shadow_program = ctx.program(vertex_shader=SHADOW_VERTEX_SHADER,
                                     fragment_shader=SHADOW_FRAGMENT_SHADER)
        _set_matrix(shadow_program, "light_vp", light_vp)
        ctx.disable(moderngl.CULL_FACE)   # 双面写入，取最近深度，避免自阴影
        for item, normals, model, vbo, ibo, small, _ in solid:
            pos_vbo = ctx.buffer(item.positions.astype(np.float32).tobytes())
            vao = ctx.vertex_array(
                shadow_program, [(pos_vbo, "3f", "in_pos")], ibo,
                index_element_size=2 if small else 4)
            _set_matrix(shadow_program, "model", model)
            vao.render()
        ctx.disable(moderngl.CULL_FACE)
        shadow_tex.compare_func = ""      # 采样时要深度值，不要 0/1 比较结果
        fbo.use()

    for entry in solid:
        draw(entry)

    if blended:
        # 特效（发光、玻璃）走混合通道：不写深度，按材质混合模式叠加
        ctx.enable(moderngl.BLEND)
        ctx.depth_mask = False
        for entry in blended:
            mode = entry[0].blend_mode
            if mode == "additive":
                ctx.blend_func = (moderngl.SRC_ALPHA, moderngl.ONE)
            elif mode == "modulate":
                ctx.blend_func = (moderngl.DST_COLOR, moderngl.ZERO)
            else:
                ctx.blend_func = (moderngl.SRC_ALPHA, moderngl.ONE_MINUS_SRC_ALPHA)
            draw(entry)
        ctx.depth_mask = True
        ctx.disable(moderngl.BLEND)

    # 后处理：亮部提取 -> 两趟高斯 -> 合成（曝光 + sRGB）
    screen = ctx.simple_framebuffer((size, size))
    screen.use()
    ctx.disable(moderngl.DEPTH_TEST)
    quad_positions = np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype="f4")
    quad_buffer = ctx.buffer(quad_positions.tobytes())

    composite = ctx.program(vertex_shader=POST_VERTEX_SHADER,
                            fragment_shader=COMPOSITE_FRAGMENT_SHADER)
    composite_vao = ctx.vertex_array(composite, [(quad_buffer, "2f", "in_pos")])
    bloom_texture = ctx.texture((1, 1), 4, bytes((0, 0, 0, 0)))

    if bloom:
        half = max(size // 2, 1)
        bright = ctx.texture((half, half), 4, dtype="f2")
        temp = ctx.texture((half, half), 4, dtype="f2")
        for target_tex in (bright, temp):
            target_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
            target_tex.repeat_x = target_tex.repeat_y = False

        bright_program = ctx.program(vertex_shader=POST_VERTEX_SHADER,
                                     fragment_shader=BRIGHT_FRAGMENT_SHADER)
        bright_vao = ctx.vertex_array(bright_program, [(quad_buffer, "2f", "in_pos")])
        bright_fbo = ctx.framebuffer([bright])
        bright_fbo.use()
        scene_texture.use(0)
        bright_program["scene"].value = 0
        _set(bright_program, "threshold", float(bloom_threshold))
        bright_vao.render(moderngl.TRIANGLE_STRIP)

        blur_program = ctx.program(vertex_shader=POST_VERTEX_SHADER,
                                   fragment_shader=BLUR_FRAGMENT_SHADER)
        blur_vao = ctx.vertex_array(blur_program, [(quad_buffer, "2f", "in_pos")])
        passes = (((1.0 / half, 0.0), bright, temp), ((0.0, 1.0 / half), temp, bright))
        for direction, source, destination in passes:
            target_fbo = ctx.framebuffer([destination])
            target_fbo.use()
            source.use(0)
            blur_program["src"].value = 0
            blur_program["direction"].value = direction
            blur_vao.render(moderngl.TRIANGLE_STRIP)
        bloom_texture = bright
        screen.use()

    # 深度差近似的环境光遮蔽：让角色与地面、舞台构件之间压出暗部
    ao_texture = ctx.texture((1, 1), 1, bytes((255,)))
    if ao:
        ao_size = max(size // 2, 1)
        ao_texture = ctx.texture((ao_size, ao_size), 1, dtype="f1")
        ao_texture.filter = (moderngl.LINEAR, moderngl.LINEAR)
        ao_texture.repeat_x = ao_texture.repeat_y = False
        ao_program = ctx.program(vertex_shader=POST_VERTEX_SHADER,
                                 fragment_shader=AO_FRAGMENT_SHADER)
        ao_vao = ctx.vertex_array(ao_program, [(quad_buffer, "2f", "in_pos")])
        ao_fbo = ctx.framebuffer([ao_texture])
        ao_fbo.use()
        scene_depth.use(0)
        ao_program["depth_tex"].value = 0
        _set(ao_program, "strength", float(ao_strength))
        ao_vao.render(moderngl.TRIANGLE_STRIP)
        screen.use()

    scene_texture.use(0)
    bloom_texture.use(1)
    ao_texture.use(2)
    composite["scene"].value = 0
    composite["bloom"].value = 1
    _set(composite, "ao", 2)
    _set(composite, "has_ao", bool(ao))
    _set(composite, "intensity", float(bloom_intensity if bloom else 0.0))
    _set(composite, "exposure", float(exposure))
    composite_vao.render(moderngl.TRIANGLE_STRIP)

    pixels = screen.read(components=3)
    image = Image.frombytes("RGB", screen.size, pixels).transpose(Image.FLIP_TOP_BOTTOM)
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
