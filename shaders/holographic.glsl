#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_strength;
uniform float u_speed;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Sweeping rainbow hue across the image
    float phase = v_uv.x * 4.0 + v_uv.y * 2.0 + u_time * u_speed * 0.5;
    float hue = fract(phase);
    // Convert hue to RGB
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(hue + K.xyz) * 6.0 - K.www);
    vec3 iris = clamp(p - K.xxx, 0.0, 1.0);
    // Screen blend: brightens and colour-tints without darkening
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - iris * u_strength * 0.65);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
