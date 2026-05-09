#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_edge_str;
uniform float u_glow_radius;
uniform float u_hue_shift;
uniform float u_bg_darken;
float sobel_lum(sampler2D tex, vec2 uv, vec2 px, out float gx, out float gy) {
    float k0 = dot(texture(tex, uv+vec2(-1,-1)*px).rgb, vec3(0.299,0.587,0.114));
    float k1 = dot(texture(tex, uv+vec2( 0,-1)*px).rgb, vec3(0.299,0.587,0.114));
    float k2 = dot(texture(tex, uv+vec2( 1,-1)*px).rgb, vec3(0.299,0.587,0.114));
    float k3 = dot(texture(tex, uv+vec2(-1, 0)*px).rgb, vec3(0.299,0.587,0.114));
    float k5 = dot(texture(tex, uv+vec2( 1, 0)*px).rgb, vec3(0.299,0.587,0.114));
    float k6 = dot(texture(tex, uv+vec2(-1, 1)*px).rgb, vec3(0.299,0.587,0.114));
    float k7 = dot(texture(tex, uv+vec2( 0, 1)*px).rgb, vec3(0.299,0.587,0.114));
    float k8 = dot(texture(tex, uv+vec2( 1, 1)*px).rgb, vec3(0.299,0.587,0.114));
    gx = -k0+k2-2.0*k3+2.0*k5-k6+k8;
    gy = -k0-2.0*k1-k2+k6+2.0*k7+k8;
    return dot(texture(tex, uv).rgb, vec3(0.299,0.587,0.114));
}
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float gx, gy;
    sobel_lum(u_tex, v_uv, px, gx, gy);
    float edge = clamp(sqrt(gx*gx+gy*gy)*u_edge_str, 0.0, 1.0);
    float edge_angle = atan(gy, gx) / 6.28318 + 0.5 + u_hue_shift;
    vec3 edge_col = hue2rgb(edge_angle);
    // Approximate glow: sample edge at 8 directions × 4 radii
    vec3 glow = vec3(0.0);
    float wsum = 0.0;
    for (int ri = 1; ri <= 8; ri++) {
        float r = float(ri) * u_glow_radius * 0.125;
        float w = exp(-r * r / (u_glow_radius * u_glow_radius * 0.15 + 0.001));
        for (int di = 0; di < 8; di++) {
            float a = float(di) * 0.7854;
            vec2 offset = vec2(cos(a), sin(a)) * r;
            vec2 uv2 = clamp(v_uv + offset * px, 0.0, 1.0);
            float gx2, gy2;
            sobel_lum(u_tex, uv2, px, gx2, gy2);
            float e2 = clamp(sqrt(gx2*gx2+gy2*gy2)*u_edge_str*0.6, 0.0, 1.0);
            float angle2 = atan(gy2, gx2) / 6.28318 + 0.5 + u_hue_shift;
            glow += hue2rgb(angle2) * e2 * w;
            wsum += w;
        }
    }
    glow /= (wsum + 0.001);
    vec4 orig = texture(u_tex, v_uv);
    vec3 bg = orig.rgb * (1.0 - u_bg_darken * 0.85);
    vec3 result = 1.0 - (1.0-bg)*(1.0-glow*3.0);
    result = mix(result, edge_col, edge * 0.85);
    frag = vec4(clamp(result, 0.0, 1.0), orig.a);
}
