#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_offset_x;
uniform float u_offset_y;
uniform float u_scale2;
uniform float u_desaturate2;
uniform float u_opacity;
void main() {
    vec4 c1 = texture(u_tex, v_uv);
    vec2 uv2 = (v_uv - 0.5) / u_scale2 + 0.5 + vec2(u_offset_x, u_offset_y);
    vec4 c2 = texture(u_tex, clamp(uv2, 0.0, 1.0));
    float lum2 = dot(c2.rgb, vec3(0.299, 0.587, 0.114));
    c2.rgb = mix(c2.rgb, vec3(lum2), u_desaturate2);
    vec3 screen = 1.0 - (1.0-c1.rgb)*(1.0-c2.rgb);
    vec3 result = mix(c1.rgb, screen, u_opacity);
    frag = vec4(clamp(result, 0.0, 1.0), c1.a);
}
