// Chrome Pulse — liquid-metal look: luma remapped through a sharp metallic
// curve, cool chrome tint, and edge glow that pulses with time.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_time;
uniform float u_chrome;  // 0..1 metallization
uniform float u_pulse;   // 0..4 pulse speed
uniform float u_edge;    // 0..1 edge glow strength

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    // Metallic transfer curve: multiple reflections along the tone ramp.
    float m = 0.5 + 0.5 * sin(lum * 12.0 - 1.5);
    m = mix(lum, m * smoothstep(0.05, 0.9, lum), 0.85);
    vec3 chrome = vec3(m) * vec3(0.85, 0.92, 1.05);
    // Edge detect for rim glow.
    float l1 = dot(texture(u_tex, v_uv + vec2(px.x * 2.0, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
    float l2 = dot(texture(u_tex, v_uv + vec2(0.0, px.y * 2.0)).rgb, vec3(0.299, 0.587, 0.114));
    float edge = clamp(abs(lum - l1) + abs(lum - l2), 0.0, 1.0) * 4.0;
    float beat = 0.6 + 0.4 * sin(u_time * u_pulse * 2.0);
    vec3 glow = vec3(0.4, 0.8, 1.0) * edge * u_edge * beat;
    vec3 rgb = mix(c.rgb, chrome, u_chrome) + glow;
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
