#version 450

// Fullscreen triangle generated from gl_VertexIndex (no vertex buffer). Covers
// the viewport; v_uv spans 0..1 across it (0,0 = top-left).

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0)(2,0)(0,2)
    // z = 1.0 → far plane (clip-space [0,1]); world (nearer) draws over the sky.
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
    v_uv = p;
}
