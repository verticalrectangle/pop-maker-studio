#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_flow;
uniform float u_metallic;
uniform float u_tint_r;
uniform float u_tint_g;
uniform float u_tint_b;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }
float noise2(vec2 p) {
    vec2 i=floor(p); vec2 f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    float t = u_time * 0.3;
    // Animated flow distortion
    vec2 d;
    d.x = noise2(v_uv * 3.0 + vec2(t, t * 0.7)) - 0.5;
    d.y = noise2(v_uv * 3.0 + vec2(t * 0.8 + 1.7, t * 1.1 + 3.3)) - 0.5;
    vec2 uv = v_uv + d * u_flow * 0.04;
    vec4 col = texture(u_tex, clamp(uv, 0.0, 1.0));
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Metallic specular: sharp highlight at mid-high luminance
    float spec = smoothstep(0.5, 0.85, lum) * smoothstep(1.0, 0.75, lum);
    // Reflection bands
    float band = sin(lum * 8.0 + t * 2.0) * 0.5 + 0.5;
    vec3 tint = vec3(u_tint_r, u_tint_g, u_tint_b);
    vec3 chrome = mix(vec3(lum * 0.5 + 0.1), vec3(lum * 0.9 + 0.1), band) * tint;
    chrome += spec * tint * 1.4;
    vec3 result = mix(col.rgb, chrome, u_metallic);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
