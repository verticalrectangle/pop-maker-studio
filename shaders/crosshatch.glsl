#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_thickness;
uniform float u_angle;
uniform float u_strength;
void main() {
    const float DEG2RAD = 0.017453293;
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 uv_px = v_uv * vec2(u_tex_w, u_tex_h);
    float paper = 0.95;
    float ink = 0.0;
    // 4 hatch layers at increasing darkness
    float angles[4];
    angles[0] = u_angle * DEG2RAD;
    angles[1] = angles[0] + 0.785;
    angles[2] = angles[0] + 0.393;
    angles[3] = angles[0] - 0.393;
    for (int i = 0; i < 4; i++) {
        float thresh = float(i+1) * 0.22;
        if (lum < thresh) {
            float cs = cos(angles[i]), sn = sin(angles[i]);
            float proj = cs * uv_px.x + sn * uv_px.y;
            float line = abs(fract(proj / u_density) - 0.5) * 2.0;
            float hatch = 1.0 - smoothstep(1.0 - u_thickness, 1.0, line);
            ink = max(ink, hatch);
        }
    }
    vec3 result = vec3(paper) * (1.0 - ink * 0.9);
    // Faint original color show-through
    result = mix(result, result * (col.rgb * 0.4 + 0.7), 0.25);
    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);
}
