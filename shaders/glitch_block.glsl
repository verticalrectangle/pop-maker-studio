#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main() {
    float t = floor(u_time * u_speed * 8.0);
    // Random blocks: divide screen into chunks
    vec2 block = floor(v_uv * vec2(12.0, 20.0));
    float r = hash(block + vec2(t, t * 0.7));
    // Only displace when random value exceeds threshold
    float blk = step(1.0 - u_amount * 0.8, r);
    float shift = (hash(block + vec2(t*1.3, 0.0)) * 2.0 - 1.0) * blk * 0.18 * u_amount;
    vec2 offset = vec2(shift, 0.0);
    frag = texture(u_tex, v_uv + offset);
}
