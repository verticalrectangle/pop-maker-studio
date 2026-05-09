#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_vignette;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    lum = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Rich warm sepia tones
    vec3 sepia = vec3(
        lum * 1.08 + 0.05,
        lum * 0.88 + 0.02,
        lum * 0.62
    );
    vec3 result = mix(col.rgb, sepia, u_strength);
    // Vignette
    vec2 d = (v_uv - 0.5) * vec2(1.0, 1.3);
    float vig = 1.0 - smoothstep(0.2, 0.7, length(d)) * u_vignette;
    result *= vig;
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
