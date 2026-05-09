#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_shadow_hue;
uniform float u_hi_hue;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Convert hue to color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 ps = abs(fract(u_shadow_hue + K.xyz) * 6.0 - K.www);
    vec3 ph = abs(fract(u_hi_hue     + K.xyz) * 6.0 - K.www);
    vec3 shadow_col = clamp(ps - K.xxx, 0.0, 1.0);
    vec3 hi_col     = clamp(ph - K.xxx, 0.0, 1.0);
    // Blend by luminance zone
    float shadow_wt = smoothstep(0.5, 0.0, lum);
    float hi_wt     = smoothstep(0.5, 1.0, lum);
    vec3 tone = col.rgb
              + shadow_col * shadow_wt * u_strength * 0.4
              - (1.0 - shadow_col) * shadow_wt * u_strength * 0.1
              + hi_col * hi_wt * u_strength * 0.35;
    frag = vec4(clamp(tone, 0.0, 1.0), col.a);
}
