#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_contrast;
uniform float u_blue_tint;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Invert
    float inv = 1.0 - lum;
    // Contrast push
    inv = clamp((inv - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Blue-white x-ray tinting: shadows blue-grey, highlights white
    vec3 xray = mix(
        vec3(0.55, 0.65, 0.85),   // shadow blue
        vec3(0.95, 0.97, 1.0),    // highlight white
        inv
    );
    xray = mix(vec3(inv), xray, u_blue_tint);
    frag = vec4(clamp(mix(col.rgb, xray, u_strength), 0.0, 1.0), col.a);
}
