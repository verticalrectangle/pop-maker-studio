#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tone;
uniform float u_vignette;
uniform float u_scratch;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // High contrast
    lum = clamp((lum - 0.5) * 1.4 + 0.5, 0.0, 1.0);
    // Sepia / silver-brown tone
    vec3 warm = vec3(0.85, 0.75, 0.55);
    vec3 cool = vec3(0.7, 0.75, 0.85);
    vec3 toned = mix(cool * lum, warm * lum, u_tone);
    // Heavy vignette (oval plate edges)
    vec2 d = (v_uv - 0.5) * vec2(1.0, 1.3);
    float vig = 1.0 - smoothstep(0.25, 0.75, length(d)) * u_vignette;
    toned *= vig;
    // Random scratches
    float scratch_n = hash(vec2(floor(v_uv.x * 400.0) / 400.0, u_time * 0.1));
    float scratch = step(1.0 - u_scratch * 0.04, scratch_n)
                  * smoothstep(0.4, 0.6, v_uv.y);
    toned += scratch * 0.4;
    // Silver plate texture noise
    float plate = hash(v_uv * 500.0) * 0.04 - 0.02;
    frag = vec4(clamp(toned + plate, 0.0, 1.0), col.a);
}
