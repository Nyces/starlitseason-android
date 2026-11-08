#version 450

// 单光源 Lambert + 环境项。背面自动翻转法线，所以网格不需要严格的外向法线。

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_color;

layout(set = 0, binding = 0) uniform CameraBlock {
    mat4 view_projection;
    vec4 light_direction;
} camera;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 normal = normalize(v_normal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    vec3 light = normalize(camera.light_direction.xyz);
    float diffuse = max(dot(normal, light), 0.0);
    vec3 color = v_color * (0.34 + 0.66 * diffuse);
    out_color = vec4(color, 1.0);
}
