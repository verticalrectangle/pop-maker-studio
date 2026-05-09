#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * u_speed;
    float nx = noise2(vec2(v_uv.x*5.0, v_uv.y*10.0 + t));
    float ny = noise2(vec2(v_uv.x*5.0 + 100.0, v_uv.y*10.0 + t*1.3));
    float rise = smoothstep(0.0, 0.7, 1.0 - v_uv.y);
    vec2 warp = vec2(nx-0.5, ny-0.5) * u_intensity * 0.05 * rise;
    frag = texture(u_tex, clamp(v_uv + warp, 0.0, 1.0));
}
