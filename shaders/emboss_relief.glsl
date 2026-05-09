#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_angle;
uniform float u_colorize;
void main() {
    const float DEG2RAD = 0.017453293;
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float a = u_angle * DEG2RAD;
    vec2 light = vec2(cos(a), sin(a));
    // Sample in light direction and opposite
    vec3 fwd = texture(u_tex, clamp(v_uv + light * px * u_strength, 0.0, 1.0)).rgb;
    vec3 bwd = texture(u_tex, clamp(v_uv - light * px * u_strength, 0.0, 1.0)).rgb;
    vec3 orig = texture(u_tex, v_uv).rgb;
    float lum_fwd = dot(fwd, vec3(0.299, 0.587, 0.114));
    float lum_bwd = dot(bwd, vec3(0.299, 0.587, 0.114));
    // Emboss = difference gives raised surface effect
    float bump = (lum_fwd - lum_bwd) * 0.5 + 0.5;
    vec3 relief = vec3(bump);
    // Optional color preserve
    float lum_orig = dot(orig, vec3(0.299, 0.587, 0.114));
    vec3 colored = mix(vec3(bump), orig * (bump / max(lum_orig, 0.001)), u_colorize);
    frag = vec4(clamp(colored, 0.0, 1.0), 1.0);
}
