#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_density;
uniform float u_strength;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Scanline-based dropout
    float scan = floor(v_uv.y * u_tex_h);
    float frame = floor(u_time * u_speed * 15.0);
    float drop_r = hash(vec2(scan, frame));
    float drop_r2 = hash(vec2(scan * 0.5, frame + 1.0));
    // Dropout bands (white signal loss)
    float dropout = step(1.0 - u_density, drop_r) * u_strength;
    vec3 result = mix(col.rgb, vec3(1.0), dropout);
    // Chroma shift on dropout lines
    if (dropout > 0.0) {
        float shift = (drop_r2 - 0.5) * 0.04;
        result.r = texture(u_tex, clamp(v_uv + vec2(shift, 0.0), 0.0, 1.0)).r;
        result.b = texture(u_tex, clamp(v_uv - vec2(shift, 0.0), 0.0, 1.0)).b;
    }
    frag = vec4(result, col.a);
}
