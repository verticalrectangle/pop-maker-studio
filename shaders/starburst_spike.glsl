#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_length;
uniform float u_rays;
void main() {
    const float PI = 3.14159265;
    vec4 col = texture(u_tex, v_uv);
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    int N = int(u_rays);
    vec3 burst = vec3(0.0);
    for (int r = 0; r < N; r++) {
        float ang = PI * float(r) / float(N);
        vec2 dir = vec2(cos(ang)*ipx.x, sin(ang)*ipx.y);
        for (int s = 1; s <= 40; s++) {
            float t = float(s) / 40.0;
            if (t > u_length * 4.0) break;
            float wt = (1.0 - t) * exp(-t * 3.0);
            vec2 uv_a = v_uv + dir * float(s) * 28.0 * u_length;
            vec2 uv_b = v_uv - dir * float(s) * 28.0 * u_length;
            vec3 ca = texture(u_tex, clamp(uv_a, 0.0, 1.0)).rgb;
            vec3 cb = texture(u_tex, clamp(uv_b, 0.0, 1.0)).rgb;
            float ba = dot(ca, vec3(0.2126,0.7152,0.0722));
            float bb = dot(cb, vec3(0.2126,0.7152,0.0722));
            burst += (ca * step(u_threshold, ba) + cb * step(u_threshold, bb)) * wt;
        }
    }
    burst /= float(N) * 2.5;
    frag = vec4(clamp(col.rgb + burst, 0.0, 1.0), col.a);
}
