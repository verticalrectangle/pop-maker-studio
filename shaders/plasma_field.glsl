#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_scale;
uniform float u_speed;
uniform float u_intensity;
uniform float u_time;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(h + K.xyz) * 6.0 - K.www);
    return clamp(p - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Plasma = sum of sine waves in 2D
    float v1 = sin(v_uv.x * u_scale + t);
    float v2 = sin(v_uv.y * u_scale * 0.9 + t * 1.1);
    float v3 = sin((v_uv.x + v_uv.y) * u_scale * 0.7 + t * 0.8);
    float v4 = sin(sqrt((v_uv.x-0.5)*(v_uv.x-0.5)*u_scale*u_scale
                       +(v_uv.y-0.5)*(v_uv.y-0.5)*u_scale*u_scale) + t);
    float plasma = (v1 + v2 + v3 + v4) * 0.25;
    float hue = plasma * 0.5 + 0.5;
    vec3 plasma_col = hue2rgb(hue);
    // Screen blend plasma over image
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - plasma_col * u_intensity * 0.7);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
