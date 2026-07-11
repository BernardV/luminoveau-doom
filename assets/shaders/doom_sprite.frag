#version 450

// Sprite fragment: alpha-tested cutout (Doom sprites are 1-bit alpha), modulated
// by the sector-light shade. Discard transparent texels so no sorting is needed.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;
layout(location = 2) in float v_dist;
layout(location = 3) in vec3  v_worldpos;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D spr_tex;

// Muzzle-flash scalar + dynamic coloured point lights (see doom_world.frag).
#define DG_MAX_LIGHTS 16
layout(set = 3, binding = 0) uniform Lighting {
    vec4  lightPosRad[DG_MAX_LIGHTS];
    vec4  lightColor[DG_MAX_LIGHTS];
    float flash;
    int   count;
    float authentic;
    float intensity;
} u;

vec3 dynamicLights(vec3 p) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < u.count; i++) {
        float rad = u.lightPosRad[i].w;
        float d   = length(u.lightPosRad[i].xyz - p);
        float a   = 1.0 - clamp(d / rad, 0.0, 1.0);
        acc += u.lightColor[i].rgb * (a * a * (3.0 - 2.0 * a));   // smoothstep, 0 at d==rad
    }
    return acc * u.intensity;
}

void main() {
    vec4 t = texture(spr_tex, v_uv);
    if (t.a < 0.5) discard;
    float dim = clamp(1.25 - v_dist / 2200.0, 0.25, 1.0);
    float flashAdd = u.flash * clamp(1.0 - v_dist / 500.0, 0.0, 1.0);
    float bright = min(v_shade * dim + flashAdd, 1.0);
    if (u.authentic > 0.5) bright = floor(bright * 16.0 + 0.5) / 16.0;
    vec3 lit = t.rgb * bright + t.rgb * dynamicLights(v_worldpos);
    out_color = vec4(min(lit, vec3(1.0)), 1.0);
}
