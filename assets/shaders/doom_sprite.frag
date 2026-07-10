#version 450

// Sprite fragment: alpha-tested cutout (Doom sprites are 1-bit alpha), modulated
// by the sector-light shade. Discard transparent texels so no sorting is needed.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D spr_tex;

void main() {
    vec4 t = texture(spr_tex, v_uv);
    if (t.a < 0.5) discard;
    out_color = vec4(t.rgb * v_shade, 1.0);
}
