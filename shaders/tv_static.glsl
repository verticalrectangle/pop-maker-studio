#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_amount;
uniform float u_color_mix;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Animated static noise
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h) / 2.0) * 2.0;
    float n = hash(npx + fract(vec2(u_time * 37.4, u_time * 23.1)));
    float n2 = hash(npx * 0.5 + fract(vec2(u_time * 11.7, u_time * 41.3)));
    // Mix grey and colored static
    vec3 grey_static = vec3(n);
    vec3 color_static = vec3(n, n2, hash(npx + 50.0 + fract(u_time * 19.3)));
    vec3 static_col = mix(grey_static, color_static, u_color_mix);
    // Blend static over image
    vec3 result = mix(col.rgb, static_col, u_amount);
    // Add horizontal roll bar occasionally
    float roll = fract(u_time * 0.08);
    float bar = smoothstep(0.02, 0.0, abs(v_uv.y - roll)) * 0.3;
    result = mix(result, vec3(1.0), bar);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
