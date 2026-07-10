#version 450

// Fullscreen triangle generated from gl_VertexIndex (no vertex buffer). Covers
// the viewport; v_uv spans 0..1 across it (0,0 = top-left).

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0)(2,0)(0,2)
    // Just under the far plane (clip [0,1]) so it PASSES the LESS depth test vs
    // the cleared depth (1.0) yet sits behind all world geometry, which then
    // draws over it. (z exactly 1.0 would fail LESS and the sky would vanish.)
    gl_Position = vec4(p * 2.0 - 1.0, 0.99999, 1.0);
    v_uv = p;
}
