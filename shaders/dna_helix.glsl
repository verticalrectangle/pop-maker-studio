#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_grid_scale;
uniform float u_wave_amp;
uniform float u_line_width;
uniform float u_hue;
uniform float u_bg_darken;
uniform float u_time;
uniform float u_strength;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * 0.8;
    vec2 uv = v_uv * u_grid_scale;
    float row = floor(uv.y);
    float cell_y = fract(uv.y);
    // Alternating sine waves for 2 strands
    float phase1 = row * 0.5 + t;
    float phase2 = row * 0.5 + t + 3.14159;
    float wave1 = sin(uv.x * 0.6 + phase1) * u_wave_amp;
    float wave2 = sin(uv.x * 0.6 + phase2) * u_wave_amp;
    // Map to cell_y [0,1]
    float cy1 = (wave1 + 1.0) * 0.5;
    float cy2 = (wave2 + 1.0) * 0.5;
    float d1 = abs(cell_y - cy1);
    float d2 = abs(cell_y - cy2);
    float strand = smoothstep(u_line_width, 0.0, min(d1, d2));
    // Connecting rungs between strands
    float rung_phase = fract(uv.x * 0.3 + t * 0.2);
    float rung = step(0.48, rung_phase) * step(rung_phase, 0.52);
    float rung_line = rung * smoothstep(u_line_width*2.0, 0.0, abs(cell_y - mix(cy1, cy2, 0.5)));
    float overlay = max(strand, rung_line * 0.6);
    // Hue varies along the helix
    float hv = fract(u_hue + uv.x * 0.03 + row * 0.07);
    vec3 line_col = hue2rgb(hv);
    vec3 bg = col.rgb * (1.0 - u_bg_darken * 0.5);
    vec3 result = mix(bg, line_col, overlay);
    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);
}
