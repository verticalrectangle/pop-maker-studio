#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_dot_size;
uniform float u_ink_threshold;
uniform float u_color_levels;
uniform float u_strength;
void main() {
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    // Snap to dot grid
    vec2 cell = floor(v_uv / (px * u_dot_size)) * px * u_dot_size;
    vec2 cell_center = cell + px * u_dot_size * 0.5;
    vec3 cell_col = texture(u_tex, clamp(cell_center, 0.0, 1.0)).rgb;
    float lum = dot(cell_col, vec3(0.299, 0.587, 0.114));
    // Dot radius scales with brightness
    float dot_r = lum * 0.55;
    vec2 local = (v_uv - cell_center) / (px * u_dot_size);
    float in_dot = step(length(local), dot_r);
    // Posterize cell color
    cell_col = floor(cell_col * u_color_levels + 0.5) / u_color_levels;
    // Ink outline from Sobel
    vec3 gx = texture(u_tex, v_uv + vec2( px.x, 0)).rgb
             -texture(u_tex, v_uv - vec2( px.x, 0)).rgb;
    vec3 gy = texture(u_tex, v_uv + vec2(0,  px.y)).rgb
             -texture(u_tex, v_uv - vec2(0,  px.y)).rgb;
    float edge = clamp((length(gx)+length(gy) - u_ink_threshold) * 8.0, 0.0, 1.0);
    vec3 result = mix(vec3(1.0), cell_col, in_dot) * (1.0 - edge);
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(clamp(mix(orig.rgb, result, u_strength), 0.0, 1.0), orig.a);
}
