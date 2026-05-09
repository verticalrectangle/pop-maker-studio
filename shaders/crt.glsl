#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_h;
uniform float u_curvature;
uniform float u_glow;
void main() {
    // Barrel warp
    vec2 p = v_uv * 2.0 - 1.0;
    p += p * p.yx * p.yx * u_curvature * 0.3;
    vec2 warped = p * 0.5 + 0.5;
    if (warped.x < 0.0 || warped.x > 1.0 || warped.y < 0.0 || warped.y > 1.0) {
        frag = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec4 col = texture(u_tex, warped);
    // Scanlines
    float line = mod(warped.y * u_tex_h, 2.0);
    float scan = mix(1.0, 0.65, step(1.0, line));
    col.rgb *= scan;
    // Phosphor glow: subtle green channel lift
    col.rgb += col.rgb * u_glow * vec3(0.05, 0.15, 0.05);
    // Screen-edge vignette
    vec2 edge = smoothstep(0.0, 0.05, warped) * smoothstep(1.0, 0.95, warped);
    col.rgb *= edge.x * edge.y;
    frag = col;
}
