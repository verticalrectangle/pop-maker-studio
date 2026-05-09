#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;
float hash(float n) { return fract(sin(n) * 43758.5453); }
void main() {
    float line = floor(v_uv.y * u_tex_h);
    float field = mod(line, 2.0);
    // Animate which lines glitch
    float t = floor(u_time * u_speed * 8.0);
    float glitch = hash(line * 0.1 + t);
    float shift = (field * 2.0 - 1.0) * u_strength * step(1.0 - u_intensity * 0.3, glitch);
    vec2 uv = clamp(v_uv + vec2(shift, 0.0), 0.0, 1.0);
    vec4 col = texture(u_tex, uv);
    // Alternate field brightness difference
    float bright = 1.0 + (field - 0.5) * 0.06 * u_intensity;
    frag = vec4(clamp(col.rgb * bright, 0.0, 1.0), col.a);
}
