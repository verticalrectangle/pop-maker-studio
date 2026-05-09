#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_threshold;
uniform float u_amount;
uniform float u_direction;
void main() {
    // Approximate pixel sort: sample along the sort axis,
    // displace toward brighter pixels above threshold.
    vec4 col = texture(u_tex, v_uv);
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    vec2 ipx = vec2(1.0/u_tex_w, 1.0/u_tex_h);
    vec2 axis = (u_direction < 0.5) ? vec2(ipx.x, 0.0) : vec2(0.0, ipx.y);
    // Find how far we're in a "sort run" — scan toward origin for contiguous above-threshold
    float run = 0.0;
    float max_run = 80.0 * u_amount;
    for (float i = 1.0; i <= max_run; i += 1.0) {
        vec3 s = texture(u_tex, v_uv - axis * i).rgb;
        float sl = dot(s, vec3(0.299, 0.587, 0.114));
        if (sl < u_threshold) break;
        run = i;
    }
    // If we're in a sort run, sample from a displaced position
    if (lum >= u_threshold && run > 0.0) {
        // Sorted output: sample ahead in the run to simulate sort
        float disp = run * u_amount;
        vec2 sort_uv = v_uv + axis * disp;
        frag = texture(u_tex, clamp(sort_uv, 0.0, 1.0));
    } else {
        frag = col;
    }
}
