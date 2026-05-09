#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_channel_mix;
uniform float u_glow;
uniform float u_contrast;
uniform float u_strength;
void main() {
    vec4 col = texture(u_tex, v_uv);
    // IR: green channel reads as infrared (foliage glows white)
    // Simulate by: high green = white, high blue = dark
    float ir = col.r * 0.2 + col.g * 0.7 + col.b * 0.1;  // IR channel
    float vis = dot(col.rgb, vec3(0.299, 0.587, 0.114));    // Visible
    float ir_val = mix(vis, ir, u_channel_mix);
    // Contrast pump for IR look
    ir_val = clamp((ir_val - 0.5) * u_contrast + 0.5, 0.0, 1.0);
    // Wood effect: green channel gets mapped to very bright
    float wood = smoothstep(0.4, 0.8, col.g) * (1.0 - col.r * 0.5);
    ir_val = mix(ir_val, 1.0, wood * u_channel_mix * 0.5);
    // Glow on bright areas
    vec2 px = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    float glow_acc = 0.0;
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 2.5;
        glow_acc += texture(u_tex, clamp(v_uv+vec2(r,0)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv-vec2(r,0)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv+vec2(0,r)*px,0.,1.)).g;
        glow_acc += texture(u_tex, clamp(v_uv-vec2(0,r)*px,0.,1.)).g;
    }
    glow_acc /= 16.0;
    ir_val = min(ir_val + glow_acc * u_glow * 0.3, 1.0);
    // Slight warm tone
    vec3 result = vec3(ir_val * 1.02, ir_val * 0.99, ir_val * 0.92);
    frag = vec4(clamp(mix(col.rgb, result, u_strength), 0.0, 1.0), col.a);
}
