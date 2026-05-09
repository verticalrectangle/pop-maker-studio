#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_grain;
uniform float u_gate;
uniform float u_fade;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    // Gate weave: horizontal shift per frame
    float weave = sin(u_time * 12.0) * 0.008 * u_gate;
    float weave_y = cos(u_time * 7.3) * 0.005 * u_gate;
    vec2 uv = v_uv + vec2(weave, weave_y);
    vec4 col = texture(u_tex, clamp(uv, 0.0, 1.0));
    // Warm color fade (kodachrome-ish)
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec3 warm = col.rgb * vec3(1.1, 0.98, 0.8) + vec3(u_fade*0.12, u_fade*0.05, 0.0);
    // Lift shadows (fade blacks)
    warm = mix(warm, warm + vec3(0.08, 0.06, 0.04) * u_fade, smoothstep(0.3, 0.0, lum));
    // Grain
    float grain = (hash(uv * 800.0 + fract(u_time * 24.0)) - 0.5) * u_grain * 0.12;
    warm += grain;
    // Sprocket hole vignette (narrow frame)
    float frame_v = smoothstep(0.0, 0.04, v_uv.y) * smoothstep(1.0, 0.96, v_uv.y);
    float frame_h = smoothstep(0.0, 0.03, v_uv.x) * smoothstep(1.0, 0.97, v_uv.x);
    warm *= frame_v * frame_h;
    frag = vec4(clamp(warm, 0.0, 1.0), col.a);
}
