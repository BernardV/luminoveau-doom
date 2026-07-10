#version 450

// Overlay fragment: alpha-tested cutout (weapon/HUD sprites), no lighting.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D ovl_tex;

void main() {
    vec4 t = texture(ovl_tex, v_uv);
    if (t.a < 0.5) discard;
    out_color = vec4(t.rgb, 1.0);
}
