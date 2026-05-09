#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_width;
void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    vec4 col = texture(u_tex, v_uv);
    // Accumulate oversaturated blur (source of the glow halos)
    float r = u_width;
    vec3 bloom = vec3(0.0);
    float samples = 0.0;
    for (float dy = -r; dy <= r; dy += 1.0) {
        for (float dx = -r; dx <= r; dx += 1.0) {
            vec4 s = texture(u_tex, v_uv + vec2(dx, dy) * px);
            float lum = dot(s.rgb, vec3(0.299, 0.587, 0.114));
            // Boost saturation of each sample before blurring
            bloom += mix(vec3(lum), s.rgb, 2.5);
            samples += 1.0;
        }
    }
    bloom = max(bloom / samples, 0.0);
    // Screen blend the saturated bloom over the source
    vec3 screen = 1.0 - (1.0 - col.rgb) * (1.0 - bloom * u_strength * 0.8);
    frag = vec4(clamp(screen, 0.0, 1.0), col.a);
}
