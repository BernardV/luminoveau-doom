#version 450

// Bloom composite: sample the offscreen 3D scene, add a soft glow extracted from
// its bright regions (lights, sky, lava, muzzle flash), and write to the target.
// strength == 0 → exact passthrough (Authentic+ / crisp mode, no bloom).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D scene_tex;

layout(set = 3, binding = 0) uniform BloomParams {
    vec2  texel;      // 1/width, 1/height of scene_tex (blur step size)
    float strength;   // glow amount; 0 disables (crisp mode)
    float threshold;  // brightness cutoff for the bright-pass
} p;

// Keep only the portion of a texel's colour above the brightness threshold.
vec3 brightPass(vec2 uv) {
    vec3 c = texture(scene_tex, uv).rgb;
    float l = max(max(c.r, c.g), c.b);
    float k = max(l - p.threshold, 0.0) / max(l, 1e-4);
    return c * k;
}

void main() {
    vec3 base = texture(scene_tex, v_uv).rgb;
    if (p.strength <= 0.0) { out_color = vec4(base, 1.0); return; }

    // Cheap two-ring radial blur of the bright-pass — enough for a soft halo
    // without a separable/downsampled chain.
    vec3  sum  = brightPass(v_uv) * 4.0;
    float wsum = 4.0;
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float a = 6.2831853 * float(i) / float(N);
        vec2 dir = vec2(cos(a), sin(a));
        sum += brightPass(v_uv + dir * p.texel * 3.0) * 2.0; wsum += 2.0;   // inner
        sum += brightPass(v_uv + dir * p.texel * 6.0) * 1.0; wsum += 1.0;   // outer
    }
    vec3 bloom = sum / wsum;
    out_color = vec4(base + bloom * p.strength, 1.0);
}
