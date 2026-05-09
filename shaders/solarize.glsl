#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_threshold;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // Sabattier: invert channels that exceed threshold
    vec3 inv = 1.0 - col.rgb;
    // Smooth inversion using a tent function around threshold
    vec3 solar = mix(col.rgb,
                     mix(col.rgb, inv, step(u_threshold, col.rgb)),
                     u_strength);
    // Enhance the characteristic solarization colors
    float lum = dot(solar, vec3(0.299, 0.587, 0.114));
    solar = mix(vec3(lum), solar, 1.6); // boost saturation
    frag = vec4(clamp(solar, 0.0, 1.0), col.a);
}
