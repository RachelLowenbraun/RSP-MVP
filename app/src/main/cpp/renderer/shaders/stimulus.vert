#version 450
// Fullscreen triangle without vertex buffer — gl_VertexIndex generates UV.
layout(location = 0) out vec2 v_uv;
void main() {
    // Standard fullscreen-triangle trick.
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
