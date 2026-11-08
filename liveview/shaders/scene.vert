#version 450

// 所有顶点在 CPU 侧已经变换到世界坐标（舞台烘好放置矩阵，角色逐帧蒙皮）。
// 这里只做视图投影变换，法线与颜色传给片元着色器。

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(set = 0, binding = 0) uniform CameraBlock {
    mat4 view_projection;
    vec4 light_direction;
} camera;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_color;

void main() {
    gl_Position = camera.view_projection * vec4(in_position, 1.0);
    v_normal = in_normal;
    v_color = in_color;
}
