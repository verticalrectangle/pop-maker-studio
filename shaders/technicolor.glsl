#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_saturation;
uniform float u_contrast;
uniform float u_warmth;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Aggressive saturation push
    vec3 sat = mix(vec3(lum), col.rgb, u_saturation);
    // Contrast S-curve
    sat = clamp((sat - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Warm shift (Technicolor skewed warm)
    sat.r = min(sat.r * (1.0 + u_warmth * 0.25), 1.0);
    sat.b = sat.b * (1.0 - u_warmth * 0.15);
    // Slight red/cyan split to simulate 3-strip registration
    float r = texture(u_tex, clamp(v_uv + vec2(0.002, 0.0), 0.0, 1.0)).r;
    float lum2 = dot(sat, vec3(0.299, 0.587, 0.114));
    sat.r = mix(sat.r, pow(r * (1.0 + u_warmth*0.3), 0.9), 0.3);
    frag = vec4(clamp(mix(col.rgb, sat, u_strength), 0.0, 1.0), col.a);
}
