#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_focus_y;
uniform float u_focus_band;
uniform float u_blur_radius;
uniform float u_saturation;
void main() {
    float dist = abs(v_uv.y - u_focus_y);
    float blur_t = smoothstep(u_focus_band, u_focus_band + 0.25, dist);
    // Blur using fixed UV offsets (no variable loop bounds — avoids Mesa issues)
    // offsets scaled from 0 to blur_radius * 0.005 UV
    float maxOff = u_blur_radius * 0.005 * blur_t;
    float o1 = maxOff * 0.25;
    float o2 = maxOff * 0.5;
    float o3 = maxOff * 0.75;
    float o4 = maxOff;
    float o5 = maxOff * 1.33;
    vec4 c  = texture(u_tex, v_uv);
    vec4 y1 = texture(u_tex, clamp(v_uv + vec2(0, o1), 0.0, 1.0));
    vec4 y2 = texture(u_tex, clamp(v_uv - vec2(0, o1), 0.0, 1.0));
    vec4 y3 = texture(u_tex, clamp(v_uv + vec2(0, o2), 0.0, 1.0));
    vec4 y4 = texture(u_tex, clamp(v_uv - vec2(0, o2), 0.0, 1.0));
    vec4 y5 = texture(u_tex, clamp(v_uv + vec2(0, o3), 0.0, 1.0));
    vec4 y6 = texture(u_tex, clamp(v_uv - vec2(0, o3), 0.0, 1.0));
    vec4 y7 = texture(u_tex, clamp(v_uv + vec2(0, o4), 0.0, 1.0));
    vec4 y8 = texture(u_tex, clamp(v_uv - vec2(0, o4), 0.0, 1.0));
    vec4 y9 = texture(u_tex, clamp(v_uv + vec2(0, o5), 0.0, 1.0));
    vec4 yA = texture(u_tex, clamp(v_uv - vec2(0, o5), 0.0, 1.0));
    vec4 blurred = (c*0.20 + (y1+y2)*0.17 + (y3+y4)*0.13 + (y5+y6)*0.09 + (y7+y8)*0.06 + (y9+yA)*0.04);
    blurred /= (0.20 + 2.0*(0.17+0.13+0.09+0.06+0.04));
    vec4 orig = texture(u_tex, v_uv);
    vec4 mixed = mix(orig, blurred, blur_t);
    float lum = dot(mixed.rgb, vec3(0.299, 0.587, 0.114));
    mixed.rgb = mix(vec3(lum), mixed.rgb, u_saturation);
    frag = vec4(clamp(mixed.rgb, 0.0, 1.0), mixed.a);
}
