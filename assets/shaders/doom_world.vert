#version 450

// Fase 1: transform wall vertices (engine-space position + a grayscale shade)
// by the camera MVP. SDL_shadercross convention: vertex uniform buffers live in
// descriptor set 1.

layout(location = 0) in vec3  in_pos;
layout(location = 1) in float in_shade;

layout(location = 0) out float v_shade;

layout(set = 1, binding = 0) uniform ViewProj {
    mat4 mvp;
} vp;

void main() {
    gl_Position = vp.mvp * vec4(in_pos, 1.0);
    v_shade = in_shade;
}
