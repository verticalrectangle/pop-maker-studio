#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_density;
uniform float u_size;
uniform float u_sparkle;
uniform float u_color_var;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0,2.0/3.0,1.0/3.0,3.0);
    return clamp(abs(fract(h+K.xyz)*6.0-K.www)-K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec3 glitter = vec3(0.0);
    // grid_scale: number of grid cells across width (sqrt of density param for intuitive control)
    float grid_scale = sqrt(u_density);
    // Sparkle radius: fraction of one grid cell
    float cell_size = 1.0 / grid_scale;
    float spark_r = cell_size * u_size * 0.25;
    for (int layer = 0; layer < 3; layer++) {
        float fl = float(layer) * 7.13 + 3.7;
        vec2 grid = floor(v_uv * grid_scale + fl);
        for (int gx = -1; gx <= 1; gx++) {
            for (int gy = -1; gy <= 1; gy++) {
                vec2 g = grid + vec2(gx, gy);
                vec2 center = (g - fl + vec2(hash(g), hash(g + vec2(5.1, 9.3)))) / grid_scale;
                // Aspect-corrected distance (so sparkles are round)
                float asp = u_tex_w / u_tex_h;
                vec2 diff = (v_uv - center) * vec2(asp, 1.0);
                float dist = length(diff);
                float phase = hash(g + fl + vec2(23.1, 41.7));
                float anim = abs(sin(u_time * 4.0 + phase * 6.28));
                float r = spark_r * (0.3 + 0.7 * anim);
                // Round core sparkle
                float spark = smoothstep(r, r * 0.05, dist) * u_sparkle * anim;
                // 4-pointed star arms
                vec2 adiff = abs(diff);
                float arm_len = r * 2.5;
                float star = smoothstep(arm_len, 0.0, min(adiff.x, adiff.y) * 2.5 + dist * 0.5) * anim * 0.4;
                float hue = hash(g + vec2(fl, fl * 1.7));
                vec3 color = mix(vec3(1.0), hue2rgb(hue), u_color_var);
                glitter += color * max(spark, star);
            }
        }
    }
    vec3 result = 1.0 - (1.0 - col.rgb) * (1.0 - clamp(glitter, 0.0, 1.0));
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
