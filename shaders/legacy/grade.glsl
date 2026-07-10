// Metal transpile source for the legacy FXType::Grade pass.
// Desktop source of truth: k_grade_frag in src/fx_shader.cpp — keep in sync.
// Uniform names match the fx_chain / set_clip_fx param keys (fill_params in
// metal_render.mm strips the u_ prefix when matching live-FX params), so the
// vignette half of the desktop program is split out into legacy/vignette.glsl.
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_brightness;
uniform float u_contrast;
uniform float u_saturation;
uniform float u_hue;

vec3 hue_rotate(vec3 c, float deg) {
    float rad = deg * 0.017453293;
    float ch = cos(rad), sh = sin(rad);
    mat3 m = mat3(
        0.299+0.701*ch+0.168*sh, 0.299-0.299*ch-0.328*sh, 0.299-0.299*ch+1.250*sh,
        0.587-0.587*ch+0.330*sh, 0.587+0.413*ch+0.035*sh, 0.587-0.587*ch-1.050*sh,
        0.114-0.114*ch-0.497*sh, 0.114-0.114*ch+0.292*sh, 0.114+0.886*ch-0.203*sh
    );
    return clamp(m * c, 0.0, 1.0);
}

void main() {
    vec4 c = texture(u_tex, v_uv);
    vec3 rgb = c.rgb + u_brightness;
    rgb = (rgb - 0.5) * u_contrast + 0.5;
    float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(lum), rgb, u_saturation);
    if (abs(u_hue) > 0.1) rgb = hue_rotate(rgb, u_hue);
    frag = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
