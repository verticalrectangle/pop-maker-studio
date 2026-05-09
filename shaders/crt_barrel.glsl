#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_distort;
uniform float u_corner_dark;
uniform float u_rgb_shift;
uniform float u_scanline;
vec2 barrel(vec2 uv, float k) {
    vec2 cc = uv - 0.5;
    float r2 = dot(cc, cc);
    return uv + cc * (r2 * k);
}
void main() {
    vec2 uv = barrel(v_uv, u_distort * 0.6);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        frag = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uvR = barrel(v_uv + vec2( u_rgb_shift, 0), u_distort*0.6);
    vec2 uvB = barrel(v_uv + vec2(-u_rgb_shift, 0), u_distort*0.6);
    float r = texture(u_tex, clamp(uvR,0.0,1.0)).r;
    float g = texture(u_tex, clamp(uv, 0.0,1.0)).g;
    float b = texture(u_tex, clamp(uvB,0.0,1.0)).b;
    vec3 col = vec3(r, g, b);
    // Scanlines
    float scan = 1.0 - u_scanline * 0.5 * (0.5 + 0.5*sin(uv.y * u_tex_h * 3.14159));
    col *= scan;
    // Vignette / corner darkening
    vec2 cc = uv - 0.5;
    float vig = 1.0 - dot(cc*1.6, cc*1.6) * u_corner_dark;
    frag = vec4(clamp(col * vig, 0.0, 1.0), 1.0);
}
