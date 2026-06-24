#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_key_r;
uniform float u_key_g;
uniform float u_key_b;
uniform float u_similarity;   // chroma distance that counts as fully keyed (transparent)
uniform float u_smoothness;   // soft edge band above the similarity cutoff
uniform float u_spill;        // suppress the key colour spilling onto the subject's edges
void main() {
    vec4 c   = texture(u_tex, v_uv);
    vec3 key = vec3(u_key_r, u_key_g, u_key_b);
    // Match in chroma space (colour minus luma) so the key is robust to how
    // brightly the screen is lit — a proper keyer, not an RGB-distance melt.
    float yk = dot(key,   vec3(0.299, 0.587, 0.114));
    float yp = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    vec2 ck  = vec2(key.r - yk, key.b - yk);
    vec2 cp  = vec2(c.r  - yp, c.b  - yp);
    float dist = length(cp - ck);
    // Transparent within `similarity`, fully opaque past `similarity + smoothness`.
    float a = smoothstep(u_similarity, u_similarity + max(u_smoothness, 1e-3), dist);
    // Despill: pull the dominant key channel down toward the other two, strongest
    // on the soft edge pixels (1 - a) where the screen colour bleeds onto hair/edges.
    vec3 rgb = c.rgb;
    float ds = u_spill * (1.0 - a);
    if (key.g >= key.r && key.g >= key.b)
        rgb.g = mix(rgb.g, min(rgb.g, max(rgb.r, rgb.b)), ds);
    else if (key.b >= key.r && key.b >= key.g)
        rgb.b = mix(rgb.b, min(rgb.b, max(rgb.r, rgb.g)), ds);
    else
        rgb.r = mix(rgb.r, min(rgb.r, max(rgb.g, rgb.b)), ds);
    frag = vec4(rgb, a * c.a);
}
