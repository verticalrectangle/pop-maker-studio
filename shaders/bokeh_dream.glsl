#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;
uniform float u_threshold;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float ar = u_tex_w / u_tex_h;
    vec3 bokeh = vec3(0.0);
    float total = 0.001;
    // Circular gather — bright pixels become bokeh discs
    int steps = 12;
    for (int a = 0; a < steps; a++) {
        float ang = 6.28318 * float(a) / float(steps);
        for (int r = 1; r <= 6; r++) {
            float rad = float(r) / 6.0 * u_radius;
            vec2 uv = v_uv + vec2(cos(ang) / ar, sin(ang)) * rad;
            vec3 s = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
            float bright = dot(s, vec3(0.2126,0.7152,0.0722));
            float wt = step(u_threshold, bright) * (1.0 - float(r)/7.0);
            bokeh += s * wt;
            total += wt;
        }
    }
    bokeh /= total;
    vec3 result = 1.0 - (1.0 - col.rgb) * (1.0 - bokeh * u_intensity * 0.5);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
