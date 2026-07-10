// HUD Glitch — cyberpunk interface overlay: cyan-shifted tint, thin HUD frame
// lines + corner ticks, and time-hashed digital block dropouts.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_time;
uniform float u_hud;      // 0..1 HUD line visibility
uniform float u_dropout;  // 0..1 block dropout rate
uniform float u_tint;     // 0..1 cyan shift

float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec2 uv = v_uv;
    float tq = floor(u_time * 6.0);
    // Digital dropouts: some 24px blocks shift and quantize.
    vec2 blk = floor(uv * vec2(u_tex_w, u_tex_h) / 24.0);
    float br = hash2(blk + tq * 2.3);
    if (br < u_dropout * 0.35) {
        uv.x += (hash2(blk.yx + tq) - 0.5) * 0.08;
        uv = clamp(uv, 0.0, 1.0);
    }
    vec4 c = texture(u_tex, uv);
    vec3 rgb = c.rgb;
    if (br < u_dropout * 0.35)
        rgb = floor(rgb * 5.0) / 5.0 * vec3(0.7, 1.1, 1.2);   // posterized cyan block
    // Cyan tint riding luma.
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(rgb, vec3(lum) * vec3(0.55, 1.0, 1.1), u_tint * 0.6);
    // HUD chrome: frame border, thirds ticks, sweeping scan bar.
    float bord = step(min(min(v_uv.x, 1.0 - v_uv.x), min(v_uv.y, 1.0 - v_uv.y)), 0.004);
    float thirds = step(abs(v_uv.x - 0.3333), 0.0008) + step(abs(v_uv.x - 0.6667), 0.0008);
    float sweep = smoothstep(0.008, 0.0, abs(v_uv.y - fract(u_time * 0.11)));
    vec3 hud_col = vec3(0.3, 1.0, 0.95);
    rgb += hud_col * (bord * 0.9 + thirds * 0.35 + sweep * 0.25) * u_hud;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
