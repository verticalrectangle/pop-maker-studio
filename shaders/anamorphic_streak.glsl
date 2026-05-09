#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_threshold;
uniform float u_length;
uniform float u_intensity;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float px = 1.0 / u_tex_w;
    vec3 streak = vec3(0.0);
    float total = 0.001;
    // Gather horizontal streak from bright neighbours
    for (int i = 1; i <= 80; i++) {
        float off = float(i) * px;
        if (off > u_length) break;
        float wt = exp(-off / u_length * 5.0);
        vec3 r = texture(u_tex, clamp(vec2(v_uv.x + off, v_uv.y), 0.0, 1.0)).rgb;
        vec3 l = texture(u_tex, clamp(vec2(v_uv.x - off, v_uv.y), 0.0, 1.0)).rgb;
        float br = max(r.r,max(r.g,r.b));
        float bl = max(l.r,max(l.g,l.b));
        float mr = step(u_threshold, br);
        float ml = step(u_threshold, bl);
        streak += (r*mr + l*ml) * wt;
        total  += (mr + ml) * wt;
    }
    streak /= total;
    // Cyan-tinted horizontal anamorphic streak
    vec3 anam = streak * vec3(0.3, 0.7, 1.5) * u_intensity;
    frag = vec4(clamp(col.rgb + anam, 0.0, 1.5), col.a);
}
