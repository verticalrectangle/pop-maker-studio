#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_speed;
uniform float u_green_mix;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Column-based rain strips
    float col_w = 12.0 / u_tex_w;
    float col_id = floor(v_uv.x / col_w);
    float col_phase = hash(vec2(col_id, 0.0));
    float drop_speed = u_speed * (0.5 + col_phase * 1.5);
    float drop_y = fract(col_phase + u_time * drop_speed * 0.15);
    // Rain streak: bright head, fading tail
    float dist_to_head = v_uv.y - drop_y;
    float rain = 0.0;
    if (dist_to_head > 0.0 && dist_to_head < 0.35) {
        float head = exp(-dist_to_head * 12.0);
        rain = head * step(col_phase, u_density);
    }
    // Bright head flash
    float head_flash = exp(-abs(v_uv.y - drop_y) * 60.0) * step(col_phase, u_density);
    // Monochrome-to-green shift
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 green_tint = vec3(lum * 0.2, lum, lum * 0.3);
    vec3 base = mix(col.rgb, green_tint, u_green_mix);
    vec3 rain_col = vec3(0.0, rain * 0.8, 0.0) + vec3(head_flash * 0.8, head_flash, head_flash * 0.8);
    frag = vec4(clamp(base + rain_col, 0.0, 1.0), col.a);
}
