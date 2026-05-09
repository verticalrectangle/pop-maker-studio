#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_scale;
uniform float u_speed;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * u_speed;
    vec2 sc = v_uv * u_scale;
    // 2 octaves of curl noise for vortex field
    float nx = noise2(sc + vec2(t * 0.3, 0.0))
             + noise2(sc * 2.0 + vec2(0.0, t * 0.4)) * 0.5;
    float ny = noise2(sc + vec2(100.0, t * 0.25))
             + noise2(sc * 2.0 + vec2(100.0, t * 0.35)) * 0.5;
    // Curl field: rotate the gradient 90 degrees
    vec2 curl = vec2(ny - 0.5, -(nx - 0.5)) * 2.0;
    vec2 uv = clamp(v_uv + curl * u_strength, 0.0, 1.0);
    frag = texture(u_tex, uv);
}
