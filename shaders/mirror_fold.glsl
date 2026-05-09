#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_axis;
uniform float u_vertical;
uniform float u_blend;
void main() {
    vec2 uv = v_uv;
    vec2 mirrored = uv;
    if (u_vertical < 0.5) {
        // Horizontal mirror (left/right)
        if (uv.x > u_axis) mirrored.x = u_axis - (uv.x - u_axis);
    } else {
        // Vertical mirror (top/bottom)
        if (uv.y > u_axis) mirrored.y = u_axis - (uv.y - u_axis);
    }
    vec4 orig = texture(u_tex, clamp(uv, 0.0, 1.0));
    vec4 fold = texture(u_tex, clamp(mirrored, 0.0, 1.0));
    frag = mix(fold, orig, u_blend);
}
