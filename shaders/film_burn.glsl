#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_edge;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float fbm(vec2 p) {
    float v = 0.0; float a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); p *= 2.1; a *= 0.5; }
    return v;
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Distance to nearest edge
    float de = min(min(v_uv.x, 1.0-v_uv.x), min(v_uv.y, 1.0-v_uv.y));
    float in_edge = smoothstep(u_edge, 0.0, de);
    // Organic noise burn pattern
    float burn_n = fbm(v_uv * 6.0 + t * 0.3);
    float burn = in_edge * (burn_n * 1.5 + 0.3) * u_intensity;
    // Burn transitions: white hot → orange → brown → char
    vec3 hot   = vec3(1.0, 0.98, 0.8);
    vec3 flame = vec3(1.0, 0.4, 0.05);
    vec3 char_col  = vec3(0.05, 0.02, 0.0);
    vec3 fire = burn < 0.4 ? mix(col.rgb, hot, burn/0.4)
              : burn < 0.7 ? mix(hot, flame, (burn-0.4)/0.3)
              :               mix(flame, char_col, (burn-0.7)/0.3);
    float alpha = burn > 0.95 ? 0.0 : col.a;
    frag = vec4(clamp(fire, 0.0, 1.0), alpha);
}
