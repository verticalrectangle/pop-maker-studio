#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_desat;
uniform float u_red;
uniform float u_crush;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Desaturate
    vec3 grey = mix(col.rgb, vec3(lum), u_desat);
    // Red channel boost (blood, danger)
    grey.r = clamp(grey.r + u_red * 0.35, 0.0, 1.0);
    grey.g = clamp(grey.g - u_red * 0.1, 0.0, 1.0);
    grey.b = clamp(grey.b - u_red * 0.15, 0.0, 1.0);
    // Crush shadows to pure black
    grey = max(grey - vec3(u_crush), vec3(0.0)) / (1.0 - u_crush);
    // Slight green-shift in midtones (sickly)
    float mid = smoothstep(0.2, 0.7, lum) * (1.0 - smoothstep(0.7, 1.0, lum));
    grey.g += mid * 0.04;
    frag = vec4(clamp(grey, 0.0, 1.0), col.a);
}
