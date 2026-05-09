#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_spread;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Edge detection for prism mask
    vec3 dx = (texture(u_tex, v_uv + vec2(ipx.x,0)).rgb
             - texture(u_tex, v_uv - vec2(ipx.x,0)).rgb);
    vec3 dy = (texture(u_tex, v_uv + vec2(0,ipx.y)).rgb
             - texture(u_tex, v_uv - vec2(0,ipx.y)).rgb);
    float edge = length(dx) + length(dy);
    // Sample 7 wavelengths of the visible spectrum
    float r = texture(u_tex, clamp(v_uv + vec2( u_spread*1.0, 0), 0.0, 1.0)).r;
    float g = col.g;
    float b = texture(u_tex, clamp(v_uv - vec2( u_spread*1.0, 0), 0.0, 1.0)).b;
    // Rainbow overlay at edges
    vec3 prism = vec3(r, g, b);
    float mask = clamp(edge * 3.0, 0.0, 1.0);
    frag = vec4(mix(col.rgb, prism, mask * u_intensity), col.a);
}
