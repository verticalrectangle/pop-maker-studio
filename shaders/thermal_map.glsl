#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_cold_hue;
uniform float u_hot_hue;
uniform float u_contrast;
uniform float u_scanlines;
vec3 hue2rgb(float h) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    return clamp(abs(fract(h + K.xyz)*6.0 - K.www) - K.xxx, 0.0, 1.0);
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // Remap with contrast
    float heat = clamp((lum - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Map heat level to 5-stop thermal palette
    vec3 thermal;
    if (heat < 0.25)
        thermal = mix(hue2rgb(u_cold_hue) * 0.3, hue2rgb(u_cold_hue), heat*4.0);
    else if (heat < 0.5)
        thermal = mix(hue2rgb(u_cold_hue), hue2rgb(mix(u_cold_hue, 0.33, 1.0)), (heat-0.25)*4.0);
    else if (heat < 0.75)
        thermal = mix(hue2rgb(0.33), hue2rgb(u_hot_hue + 0.05), (heat-0.5)*4.0);
    else
        thermal = mix(hue2rgb(u_hot_hue), vec3(1.0, 1.0, 0.9), (heat-0.75)*4.0);
    // Faint scan lines
    float scan = 1.0 - u_scanlines * 0.5 * (0.5 + 0.5*sin(v_uv.y * u_tex_h * 3.14159));
    frag = vec4(clamp(thermal * scan, 0.0, 1.0), col.a);
}
