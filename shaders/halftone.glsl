#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_size;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float luma = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 cell = vec2(u_size / u_tex_w, u_size / u_tex_h);
    vec2 local = mod(v_uv, cell) / cell - 0.5;
    float radius = (1.0 - luma) * 0.5;
    float dot_mask = smoothstep(radius + 0.02, radius - 0.02, length(local));
    float halftone = mix(luma, dot_mask, u_strength);
    frag = vec4(vec3(halftone), col.a);
}
