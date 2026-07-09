#version 450

// Fase 1: flat grayscale by sector light, so wall geometry + camera are
// verifiable before textures land (Fase 1b).

layout(location = 0) in float v_shade;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(vec3(v_shade), 1.0);
}
