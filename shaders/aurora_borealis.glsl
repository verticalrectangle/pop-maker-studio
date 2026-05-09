#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_color_shift;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(h + K.xyz) * 6.0 - K.www);
    return clamp(p - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Aurora curtains: vertical bands of color
    float n1 = noise2(vec2(v_uv.x * 3.0 + t * 0.3, t * 0.1));
    float n2 = noise2(vec2(v_uv.x * 5.0 - t * 0.2, t * 0.15 + 10.0));
    // Curtain falls from top, fades at bottom
    float curtain = smoothstep(0.7, 0.1, v_uv.y) * smoothstep(0.0, 0.3, 1.0 - v_uv.y);
    float wave = sin(v_uv.x * 8.0 + t * 0.5 + n1 * 4.0) * 0.5 + 0.5;
    float aurora_mask = wave * curtain * (n1 * 0.7 + 0.3);
    float hue = fract(u_color_shift + v_uv.x * 0.4 + n2 * 0.3 + t * 0.05);
    vec3 aurora_col = hue2rgb(hue) * vec3(0.5, 1.0, 0.8); // bias toward green/teal
    // Screen blend: aurora brightens without darkening
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - aurora_col * aurora_mask * u_intensity);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
