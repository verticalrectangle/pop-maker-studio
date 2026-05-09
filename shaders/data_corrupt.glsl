#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_block_size;
uniform float u_intensity;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Block-based corruption
    vec2 block_id = floor(v_uv * vec2(u_tex_w, u_tex_h) / u_block_size);
    float frame = floor(u_time * 12.0);
    float rnd = hash(block_id + vec2(frame * 7.3, frame * 13.1));
    float rnd2 = hash(block_id + vec2(frame * 3.7, frame * 17.9));
    float rnd3 = hash(block_id + vec2(frame * 11.1, frame * 5.3));
    if (rnd < u_density) {
        // This block is corrupted: sample from a random other block
        vec2 corrupt_block = block_id + vec2((rnd2 - 0.5) * 20.0, 0.0);
        vec2 corrupt_uv = clamp((corrupt_block * u_block_size + fract(v_uv * vec2(u_tex_w,u_tex_h)/u_block_size) * u_block_size) / vec2(u_tex_w, u_tex_h), 0.0, 1.0);
        vec3 corrupt_col = texture(u_tex, corrupt_uv).rgb;
        // Add color glitch
        corrupt_col = vec3(corrupt_col.b, corrupt_col.r, corrupt_col.g) * (0.8 + rnd3 * 0.4);
        frag = vec4(mix(col.rgb, corrupt_col, u_intensity), col.a);
    } else {
        frag = col;
    }
}
