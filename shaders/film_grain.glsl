#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_time;
uniform float u_amount;
uniform float u_size;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec4 col = texture(u_tex, v_uv);
    // Grain coords tiled at grain size
    vec2 gp = floor(v_uv * vec2(640.0, 360.0) / u_size);
    float g = hash(gp + vec2(u_time * 7.3, u_time * 3.1)) * 2.0 - 1.0;
    // Luma-weighted: grain more visible in midtones
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    float w = 1.0 - abs(luma * 2.0 - 1.0);
    frag = vec4(col.rgb + g * u_amount * w * 0.35, col.a);
}
