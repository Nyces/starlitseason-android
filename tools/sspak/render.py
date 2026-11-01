"""基于 moderngl 的最小预览渲染器（验证用，非最终引擎）。"""

from __future__ import annotations

import numpy as np

from .ue4.staticmesh import StaticMesh


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


def render_static_mesh(mesh: StaticMesh, out_path: str, size: int = 1024) -> None:
    import moderngl
    from PIL import Image

    positions = mesh.positions
    indices = mesh.indices
    normals = _smooth_normals(positions, indices)

    center = (positions.min(axis=0) + positions.max(axis=0)) * 0.5
    radius = float(np.linalg.norm(positions.max(axis=0) - positions.min(axis=0)) * 0.5) or 1.0
    eye = center + np.array([radius * 2.2, -radius * 1.6, radius * 1.2], dtype=np.float32)

    ctx = moderngl.create_standalone_context(require=330)
    fbo = ctx.simple_framebuffer((size, size))
    fbo.use()
    ctx.clear(0.05, 0.06, 0.08, 1.0)
    ctx.enable(moderngl.DEPTH_TEST)

    program = ctx.program(
        vertex_shader="""
            #version 330
            uniform mat4 mvp;
            in vec3 in_pos;
            in vec3 in_nrm;
            out vec3 v_nrm;
            out vec3 v_pos;
            void main() {
                v_nrm = in_nrm;
                v_pos = in_pos;
                gl_Position = mvp * vec4(in_pos, 1.0);
            }
        """,
        fragment_shader="""
            #version 330
            in vec3 v_nrm;
            in vec3 v_pos;
            out vec4 color;
            void main() {
                vec3 n = normalize(v_nrm);
                vec3 L1 = normalize(vec3(0.45, -0.7, 0.55));
                vec3 L2 = normalize(vec3(-0.6, 0.3, 0.2));
                float d1 = max(dot(n, L1), 0.0);
                float d2 = max(dot(n, L2), 0.0) * 0.35;
                vec3 base = vec3(0.78, 0.76, 0.72);
                vec3 c = base * (0.30 + 0.85 * d1 + d2);
                color = vec4(c, 1.0);
            }
        """,
    )

    vbo = ctx.buffer(np.hstack([positions, normals]).astype(np.float32).tobytes())
    ibo = ctx.buffer(indices.astype(np.uint16).tobytes() if len(positions) < 65535
                     else indices.astype(np.uint32).tobytes())
    element_size = 2 if len(positions) < 65535 else 4
    vao = ctx.vertex_array(
        program, [(vbo, "3f 3f", "in_pos", "in_nrm")], ibo, index_element_size=element_size)

    mvp = _perspective(45.0, 1.0, 0.05, radius * 20 + 10) @ _look_at(
        eye, center, np.array([0.0, 0.0, 1.0], dtype=np.float32))
    program["mvp"].write(np.ascontiguousarray(mvp.T).astype(np.float32).tobytes())

    vao.render()
    pixels = fbo.read(components=3)
    image = Image.frombytes("RGB", fbo.size, pixels).transpose(Image.FLIP_TOP_BOTTOM)
    image.save(out_path)
