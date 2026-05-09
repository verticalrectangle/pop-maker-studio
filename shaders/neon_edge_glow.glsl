#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_glow;
uniform float u_hue;
uniform float u_strength;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Sobel edge detection
    vec3 tl = texture(u_tex, v_uv + vec2(-px.x, -px.y)).rgb;
    vec3 tc = texture(u_tex, v_uv + vec2(    0, -px.y)).rgb;
    vec3 tr = texture(u_tex, v_uv + vec2( px.x, -px.y)).rgb;
    vec3 ml = texture(u_tex, v_uv + vec2(-px.x,     0)).rgb;
    vec3 mr = texture(u_tex, v_uv + vec2( px.x,     0)).rgb;
    vec3 bl = texture(u_tex, v_uv + vec2(-px.x,  px.y)).rgb;
    vec3 bc = texture(u_tex, v_uv + vec2(    0,  px.y)).rgb;
    vec3 br = texture(u_tex, v_uv + vec2( px.x,  px.y)).rgb;
    vec3 gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    vec3 gy = -tl - 2.0*tc - tr + bl + 2.0*bc + br;
    float edge = length(vec2(length(gx), length(gy)));
    edge = smoothstep(u_threshold, u_threshold + 0.2, edge);
    // Hue → RGB for neon color
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(u_hue + K.xyz) * 6.0 - K.www);
    vec3 neon_col = clamp(p - K.xxx, 0.0, 1.0);
    // Dark background + glowing edges
    vec4 col = texture(u_tex, v_uv);
    float dark = 0.15;
    vec3 result = col.rgb * dark + neon_col * edge * u_glow;
    // Glow bloom — add blurred edge contribution
    vec3 bloom = vec3(0.0);
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 2.0;
        bloom += neon_col * edge / (r * r + 1.0);
    }
    result += bloom * u_glow * 0.3;
    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);
}
