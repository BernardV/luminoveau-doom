#version 450

// Fase 1b: sample the wall texture and modulate by the sector-light shade.
// SDL_shadercross convention: fragment samplers live in descriptor set 2.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D wall_tex;

void main() {
    vec4 tex = texture(wall_tex, v_uv);
    out_color = vec4(tex.rgb * v_shade, 1.0);
}
