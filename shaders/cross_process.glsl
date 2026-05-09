#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
uniform float u_contrast;
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 c = col.rgb;
    // E6 in C41 cross process simulation:
    // R curve: boost highlights, crush shadows
    float r = clamp(pow(c.r * 1.2, 0.7) * 1.1, 0.0, 1.0);
    // G curve: slight S-curve
    float g = clamp((c.g - 0.5) * 1.0 * u_contrast + 0.5, 0.0, 1.0);
    // B curve: boost shadows, crush highlights (inverted feel)
    float b = clamp(1.0 - pow(1.0 - c.b, 0.6) * 0.9, 0.0, 1.0);
    // Push saturation massively
    vec3 xp = vec3(r, g, b);
    float lum = dot(xp, vec3(0.299, 0.587, 0.114));
    xp = mix(vec3(lum), xp, 2.2);
    // Contrast pump
    xp = clamp((xp - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    frag = vec4(mix(col.rgb, xp, u_strength), col.a);
}
