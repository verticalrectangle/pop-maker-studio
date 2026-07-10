// Metal transpile source for the legacy FXType::Vignette pass — the vignette
// half of desktop k_grade_frag (src/fx_shader.cpp), split so the manifest
// entry carries only the "vignette" param key.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_vignette;

void main() {
    vec4 c = texture(u_tex, v_uv);
    vec3 rgb = c.rgb;
    if (u_vignette > 0.001) {
        vec2 d = v_uv * 2.0 - 1.0;
        float vig = 1.0 - smoothstep(0.5, 1.5, length(d) * u_vignette * 1.5);
        rgb *= vig;
    }
    frag = vec4(rgb, c.a);
}
