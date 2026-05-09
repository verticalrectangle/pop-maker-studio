#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Overlay blend of luminance over colour — core of the bleach-bypass look
    vec3 overlay = mix(
        2.0 * col.rgb * luma,
        1.0 - 2.0 * (1.0 - col.rgb) * (1.0 - luma),
        step(0.5, luma)
    );
    // Desaturate the result heavily (silver-retention kills chroma)
    float ov_luma = dot(overlay, vec3(0.299, 0.587, 0.114));
    vec3 bypass = mix(vec3(ov_luma), overlay, 0.25);
    // Contrast push
    bypass = clamp((bypass - 0.5) * 1.4 + 0.5, 0.0, 1.0);
    frag = vec4(mix(col.rgb, bypass, u_strength), col.a);
}
