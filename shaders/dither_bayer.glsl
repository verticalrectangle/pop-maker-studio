#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_levels;
uniform float u_scale;
uniform float u_color;
void main() {
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    // 8x8 Bayer threshold matrix (normalized)
    float bayer8[64];
    bayer8[ 0]= 0.0; bayer8[ 1]=32.0; bayer8[ 2]= 8.0; bayer8[ 3]=40.0;
    bayer8[ 4]= 2.0; bayer8[ 5]=34.0; bayer8[ 6]=10.0; bayer8[ 7]=42.0;
    bayer8[ 8]=48.0; bayer8[ 9]=16.0; bayer8[10]=56.0; bayer8[11]=24.0;
    bayer8[12]=50.0; bayer8[13]=18.0; bayer8[14]=58.0; bayer8[15]=26.0;
    bayer8[16]=12.0; bayer8[17]=44.0; bayer8[18]= 4.0; bayer8[19]=36.0;
    bayer8[20]=14.0; bayer8[21]=46.0; bayer8[22]= 6.0; bayer8[23]=38.0;
    bayer8[24]=60.0; bayer8[25]=28.0; bayer8[26]=52.0; bayer8[27]=20.0;
    bayer8[28]=62.0; bayer8[29]=30.0; bayer8[30]=54.0; bayer8[31]=22.0;
    bayer8[32]= 3.0; bayer8[33]=35.0; bayer8[34]=11.0; bayer8[35]=43.0;
    bayer8[36]= 1.0; bayer8[37]=33.0; bayer8[38]= 9.0; bayer8[39]=41.0;
    bayer8[40]=51.0; bayer8[41]=19.0; bayer8[42]=59.0; bayer8[43]=27.0;
    bayer8[44]=49.0; bayer8[45]=17.0; bayer8[46]=57.0; bayer8[47]=25.0;
    bayer8[48]=15.0; bayer8[49]=47.0; bayer8[50]= 7.0; bayer8[51]=39.0;
    bayer8[52]=13.0; bayer8[53]=45.0; bayer8[54]= 5.0; bayer8[55]=37.0;
    bayer8[56]=63.0; bayer8[57]=31.0; bayer8[58]=55.0; bayer8[59]=23.0;
    bayer8[60]=61.0; bayer8[61]=29.0; bayer8[62]=53.0; bayer8[63]=21.0;
    int px = int(mod(v_uv.x * u_tex_w / u_scale, 8.0));
    int py = int(mod(v_uv.y * u_tex_h / u_scale, 8.0));
    float threshold = bayer8[py * 8 + px] / 64.0 - 0.5;
    float step_sz = 1.0 / max(u_levels - 1.0, 1.0);
    float dith_lum = lum + threshold * step_sz;
    float quant = floor(dith_lum / step_sz + 0.5) * step_sz;
    // Color dither or monochrome
    vec3 result = mix(vec3(quant), col.rgb * quant / max(lum, 0.001), u_color);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
