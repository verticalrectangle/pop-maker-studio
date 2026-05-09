#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_hue;
uniform float u_glow;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Convert hue to dodge color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 kp = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 dodge_col = clamp(kp - K.xxx, 0.0, 1.0);
    // Color dodge blend mode
    vec3 dodged = col.rgb / max(1.0 - dodge_col * u_amount, vec3(0.001));
    dodged = clamp(dodged, 0.0, 1.0);
    // Glow halo
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec3 bloom = vec3(0.0);
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 3.0;
        bloom += texture(u_tex, clamp(v_uv + vec2(r, 0)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv - vec2(r, 0)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv + vec2(0, r)*px, 0.0, 1.0)).rgb;
        bloom += texture(u_tex, clamp(v_uv - vec2(0, r)*px, 0.0, 1.0)).rgb;
    }
    bloom /= 16.0;
    vec3 result = mix(dodged, 1.0 - (1.0 - dodged) * (1.0 - bloom * dodge_col * u_glow), 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
