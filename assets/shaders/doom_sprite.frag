#version 450

// Sprite fragment: alpha-tested cutout (Doom sprites are 1-bit alpha), modulated
// by the sector-light shade. Discard transparent texels so no sorting is needed.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;
layout(location = 2) in float v_dist;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D spr_tex;

// Localized muzzle-flash light at the eye (see doom_world.frag).
layout(set = 3, binding = 0) uniform Light { float flash; } u;

void main() {
    vec4 t = texture(spr_tex, v_uv);
    if (t.a < 0.5) discard;
    float dim = clamp(1.25 - v_dist / 2200.0, 0.25, 1.0);
    float flashAdd = u.flash * clamp(1.0 - v_dist / 500.0, 0.0, 1.0);
    float bright = min(v_shade * dim + flashAdd, 1.0);
    out_color = vec4(t.rgb * bright, 1.0);
}
