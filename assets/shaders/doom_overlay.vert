#version 450

// Screen-space overlay (weapon sprite, HUD): positions are already in NDC
// (computed on the CPU), so no transform. Used for things that sit on top of
// the 3D view without depth or distance lighting.

layout(location = 0) in vec2 in_ndc;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = vec4(in_ndc, 0.0, 1.0);
    v_uv = in_uv;
}
