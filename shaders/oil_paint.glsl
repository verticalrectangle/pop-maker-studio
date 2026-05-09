#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_radius;
uniform float u_sharpness;
uniform float u_strength;
void main() {
    // Kuwahara filter: pick quadrant with minimum variance
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    int R = int(u_radius);
    vec3 mean[4];   float var[4];
    for (int q = 0; q < 4; q++) { mean[q] = vec3(0.0); var[q] = 0.0; }
    float cnt = 0.0;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            vec2 uv = v_uv + vec2(float(dx), float(dy)) * px;
            vec3 c = texture(u_tex, clamp(uv, 0.0, 1.0)).rgb;
            if (dx <= 0 && dy <= 0) { mean[0] += c; var[0] += dot(c,c); }
            if (dx >= 0 && dy <= 0) { mean[1] += c; var[1] += dot(c,c); }
            if (dx <= 0 && dy >= 0) { mean[2] += c; var[2] += dot(c,c); }
            if (dx >= 0 && dy >= 0) { mean[3] += c; var[3] += dot(c,c); }
        }
    }
    float n = float(R+1)*float(R+1);
    float min_var = 1e9;
    vec3 result = mean[0] / n;
    for (int q = 0; q < 4; q++) {
        mean[q] /= n;
        var[q] = var[q]/n - dot(mean[q], mean[q]);
        float v = var[q];
        if (v < min_var) { min_var = v; result = mean[q]; }
    }
    // Slight sharpness boost
    vec3 orig = texture(u_tex, v_uv).rgb;
    vec3 oil_result = clamp(result + (result - orig) * (u_sharpness * 0.05), 0.0, 1.0);
    frag = vec4(mix(orig, oil_result, u_strength), 1.0);
}
