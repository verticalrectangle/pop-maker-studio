#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_warmth;
uniform float u_glow_str;
uniform float u_shadow_lift;
uniform float u_vignette;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Warm highlight push (orange-gold)
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    float hi = smoothstep(0.45, 0.9, lum);
    col.rgb += hi * u_warmth * vec3(0.25, 0.12, -0.08);
    // Cool shadow lift (slight purple-blue in shadows)
    float sha = smoothstep(0.4, 0.0, lum);
    col.rgb += sha * u_shadow_lift * vec3(0.12, 0.1, 0.2);
    // Diffuse glow from bright areas (screen blend)
    float bloom = clamp((lum - 0.55) * 2.5, 0.0, 1.0);
    vec3 glow = col.rgb * bloom * u_glow_str * vec3(1.0, 0.85, 0.5);
    col.rgb = 1.0 - (1.0 - col.rgb) * (1.0 - glow);
    // Vignette
    vec2 uvc = v_uv - 0.5;
    float vig = 1.0 - dot(uvc, uvc) * u_vignette * 2.5;
    frag = vec4(clamp(col.rgb * vig, 0.0, 1.0), col.a);
}
