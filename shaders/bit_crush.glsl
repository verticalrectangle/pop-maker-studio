#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_levels;
uniform float u_dither;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // 4x4 Bayer matrix for dithering
    float bayer[16];
    bayer[0]= 0.0/16.0; bayer[1]= 8.0/16.0; bayer[2]= 2.0/16.0; bayer[3]=10.0/16.0;
    bayer[4]=12.0/16.0; bayer[5]= 4.0/16.0; bayer[6]=14.0/16.0; bayer[7]= 6.0/16.0;
    bayer[8]= 3.0/16.0; bayer[9]=11.0/16.0;bayer[10]= 1.0/16.0;bayer[11]= 9.0/16.0;
    bayer[12]=15.0/16.0;bayer[13]= 7.0/16.0;bayer[14]=13.0/16.0;bayer[15]= 5.0/16.0;
    int bx = int(mod(v_uv.x * u_tex_w, 4.0));
    int by = int(mod(v_uv.y * u_tex_h, 4.0));
    float threshold = bayer[by * 4 + bx] - 0.5;
    float step_size = 1.0 / max(u_levels - 1.0, 1.0);
    vec3 dithered = col.rgb + threshold * step_size * u_dither;
    vec3 crushed = floor(dithered / step_size + 0.5) * step_size;
    frag = vec4(clamp(mix(col.rgb, crushed, u_strength), 0.0, 1.0), col.a);
}
