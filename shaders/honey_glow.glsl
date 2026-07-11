// Honey Glow — golden-hour warmth + soft bloom + lifted shadows. The
// "sunset filter": bright areas bleed a honey-colored halo, shadows go warm
// instead of gray.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_glow;    // 0..1 bloom strength
uniform float u_warmth;  // 0..1 golden shift
uniform float u_lift;    // 0..1 shadow lift

void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 c = texture(u_tex, v_uv);
    // Bright-pass bloom: 12-tap wide disc of thresholded samples.
    vec3 bloom = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 2.39996;
        float r = 3.0 + 9.0 * fract(float(i) * 0.618034);
        vec3 s = texture(u_tex, v_uv + vec2(cos(a), sin(a)) * r * px).rgb;
        float b = max(max(s.r, s.g), s.b);
        bloom += s * smoothstep(0.55, 0.9, b);
    }
    bloom /= 12.0;
    vec3 honey = vec3(1.0, 0.78, 0.45);
    vec3 rgb = c.rgb + bloom * honey * u_glow * 0.9;
    // Warm grade: push highlights gold, keep blacks.
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(rgb, rgb * honey * 1.15, u_warmth * (0.3 + 0.7 * lum));
    // Lift shadows toward warm brown, filmic.
    rgb = mix(rgb, max(rgb, vec3(0.14, 0.09, 0.05)), u_lift * (1.0 - lum));
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
