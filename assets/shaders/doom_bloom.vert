#version 450

// Fullscreen triangle (no vertex buffer). Covers the viewport; v_uv spans 0..1
// across it (0,0 = top-left), matching how the scene was rendered into the
// offscreen scene texture. Sits just under the far plane so the weapon/crosshair
// overlay (drawn at z=0) passes the LESS depth test and lands on top.

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0)(2,0)(0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.99999, 1.0);
    v_uv = p;
}
