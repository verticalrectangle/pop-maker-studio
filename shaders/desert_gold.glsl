#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_warmth;
uniform float u_fade;
uniform float u_haze;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Warm golden push
    vec3 warm = col.rgb * vec3(1.0 + u_warmth*0.3, 1.0 + u_warmth*0.1, 1.0 - u_warmth*0.3);
    // Lift shadows (sun-bleached fade)
    float lift = u_fade * 0.18;
    warm = warm * (1.0 - lift) + lift;
    // Haze: push toward warm white (atmospheric scattering)
    vec3 haze_col = vec3(1.0, 0.92, 0.75);
    warm = mix(warm, haze_col, u_haze * smoothstep(0.4, 1.0, lum) * 0.5);
    // Slight orange cast in highlights
    warm.r = min(warm.r + u_warmth * 0.08, 1.0);
    frag = vec4(clamp(warm, 0.0, 1.0), col.a);
}
