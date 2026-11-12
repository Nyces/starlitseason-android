#version 450

// 卡通着色：基础色贴图 × 明暗色表（两级阶跃）+ 边缘光 + 球面贴图高光。
// 参数与 Python 预览器（tools/sspak/render.py）一致，便于两边对照。

layout(set = 0, binding = 0) uniform CameraBlock {
    mat4 view_projection;
    mat4 view;
    vec4 light_direction;
    vec4 eye_position;
} camera;

layout(set = 0, binding = 1) uniform sampler2D base_texture;
layout(set = 0, binding = 2) uniform sampler2D sphere_texture;
layout(set = 0, binding = 3) uniform sampler2D shadow_texture;

layout(push_constant) uniform MaterialBlock {
    vec4 shadow_tint;   // rgb 阴影色，w 明暗阈值
    vec4 blend;         // rgb 配色乘，w 边缘光指数
    vec4 add;           // rgb 配色加，w 阴影强度
    vec4 sphere;        // rgb 球面贴图乘色，w 取幂
    vec4 params;        // x 无光照，y 抠像阈值，z 球面强度，w 是否有球面贴图
    vec4 outline;       // 顶点着色器用（外扩宽度）
} material;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_position;
layout(location = 3) in vec3 v_normal_view;

layout(location = 0) out vec4 out_color;

vec3 to_linear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 to_srgb(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }

void main() {
    vec3 normal = normalize(v_normal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }

    vec4 texel = texture(base_texture, v_uv);
    if (material.params.y > 0.0 && texel.a < material.params.y) {
        discard;                                  // BLEND_Masked 的抠像
    }
    // 材质实例的 BlendColor / AddColor 是配色旋钮（部分服装靠它改色）
    vec3 base = to_linear(texel.rgb) * to_linear(material.blend.rgb)
                + to_linear(material.add.rgb);

    if (material.params.x < 0.5) {
        vec3 view = normalize(camera.eye_position.xyz - v_position);
        // 明暗：dot(N,L) 映射到 0..1，再按阈值做阶跃
        float shade = dot(normal, normalize(camera.light_direction.xyz)) * 0.5 + 0.5;
        shade = clamp((shade - 0.5) * material.add.w + 0.5, 0.0, 1.0);
        // ShadowTexture 的 alpha 标记哪里允许出现卡通阴影（脸/身体靠它收窄阴影）
        float mask = texture(shadow_texture, v_uv).a;
        float lit = step(material.shadow_tint.w, mix(1.0, shade, mask));
        vec3 tone = mix(material.shadow_tint.rgb, vec3(1.0), lit);
        // 球面贴图（MatCap）：视空间法线 xy 映射到球面贴图
        if (material.params.w > 0.0) {
            vec3 nv = normalize(v_normal_view);
            vec2 uv = nv.xy * vec2(0.5, -0.5) + 0.5;
            vec3 s = max(texture(sphere_texture, uv).rgb * material.sphere.rgb, vec3(0.0));
            s = pow(s, vec3(material.sphere.w)) * material.params.z;
            base += to_linear(s);
        }
        base *= tone;
        // 边缘光：pow(1-|N·V|, RimScale)
        float rim = pow(clamp(1.0 - abs(dot(normal, view)), 0.0, 1.0), material.blend.w);
        base += rim;
    }

    // 目标格式自带 sRGB 编码时直接输出线性值，避免二次转换
    vec3 encoded = camera.light_direction.w > 0.5 ? clamp(base, 0.0, 1.0) : to_srgb(base);
    out_color = vec4(encoded, texel.a);
}
