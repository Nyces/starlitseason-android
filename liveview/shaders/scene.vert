#version 450

// 所有顶点在 CPU 侧已经变换到世界坐标（舞台烘好放置矩阵，角色逐帧蒙皮）。
// 这里做视图投影变换，并把法线/UV/世界位置传给片元着色器。
// 描边壳材质沿法线外扩（宽度 × 视距），配合正面剔除只留一圈轮廓。

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(set = 0, binding = 0) uniform CameraBlock {
    mat4 view_projection;
    mat4 view;
    vec4 light_direction;
    vec4 eye_position;
} camera;

layout(push_constant) uniform MaterialBlock {
    vec4 shadow_tint;
    vec4 blend;
    vec4 add;
    vec4 sphere;
    vec4 params;
    vec4 outline;       // x 外扩宽度（0 = 不扩）
} material;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_position;
layout(location = 3) out vec3 v_normal_view;

void main() {
    vec3 world = in_position;
    if (material.outline.x > 0.0) {
        vec3 normal = normalize(in_normal);
        world += normal * material.outline.x
                 * length(camera.eye_position.xyz - in_position);
    }
    gl_Position = camera.view_projection * vec4(world, 1.0);
    v_normal = in_normal;
    v_uv = in_uv;
    v_position = in_position;
    // 视空间法线给球面贴图（眼睛高光）用
    v_normal_view = mat3(camera.view) * in_normal;
}
