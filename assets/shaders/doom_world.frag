#version 450

// Fase 1b: sample the wall texture and modulate by the sector-light shade.
// SDL_shadercross convention: fragment samplers live in descriptor set 2.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;
layout(location = 2) in float v_dist;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D wall_tex;

// Muzzle-flash / light-amp as a localized point light at the eye. v_dist is the
// distance from the camera, so a simple falloff lights near surfaces and leaves
// distant ones dark — unlike Doom's flat global extralight.
layout(set = 3, binding = 0) uniform Light { float flash; } u;

void main() {
    vec4 tex = texture(wall_tex, v_uv);
    // Doom-like distance diminishing: brighter up close, fading with range,
    // floored so it never goes fully black. Combined with sector-light shade.
    float dim = clamp(1.25 - v_dist / 2200.0, 0.25, 1.0);
    float flashAdd = u.flash * clamp(1.0 - v_dist / 500.0, 0.0, 1.0);
    float bright = min(v_shade * dim + flashAdd, 1.0);
    out_color = vec4(tex.rgb * bright, 1.0);
}
