#version 450

// Fase 0 scaffolding shader: proves a game-side custom RenderPass can drive its
// own pipeline from GLSL that the engine auto-transpiles to every backend
// (SPIRV / Metal / WGSL). Draws a plain colored triangle in NDC — no camera yet.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec3 in_col;

layout(location = 0) out vec3 v_col;

void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
    v_col = in_col;
}
