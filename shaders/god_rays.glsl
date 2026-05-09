#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_decay;
uniform float u_cx;
uniform float u_cy;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 src = vec2(u_cx, u_cy);
    vec2 delta = (v_uv - src) / 16.0;
    vec2 uv = v_uv;
    vec3 rays = vec3(0.0);
    float illum = 1.0;
    for (int i = 0; i < 16; i++) {
        uv -= delta;
        vec3 s = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
        float bright = dot(s, vec3(0.2126, 0.7152, 0.0722));
        rays += s * max(bright - 0.3, 0.0) * illum;
        illum *= u_decay;
    }
    rays /= 16.0;
    frag = vec4(clamp(col.rgb + rays * u_intensity, 0.0, 1.0), col.a);
}
