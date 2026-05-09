#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_intensity;
uniform float u_color_sep;
uniform float u_luma_bias;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec4 col = texture(u_tex, v_uv);
    vec2 npx = floor(v_uv * vec2(u_tex_w, u_tex_h));
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Noise stronger in darks (shadow noise)
    float noise_scale = mix(1.0, 1.0 - lum, u_luma_bias) * u_intensity;
    float nr = (hash(npx + vec2(u_time * 0.3, 0.0)) - 0.5) * noise_scale;
    float ng = (hash(npx + vec2(0.0, u_time * 0.4) + vec2(31.7, 71.3)) - 0.5) * noise_scale;
    float nb_n = (hash(npx + vec2(u_time * 0.5) + vec2(97.1, 13.7)) - 0.5) * noise_scale;
    // Mix RGB and luminance noise
    float luma_noise = (hash(npx + vec2(u_time * 0.35, u_time * 0.25)) - 0.5) * noise_scale;
    vec3 noise = mix(vec3(luma_noise), vec3(nr, ng, nb_n), u_color_sep);
    frag = vec4(clamp(col.rgb + noise, 0.0, 1.0), col.a);
}
