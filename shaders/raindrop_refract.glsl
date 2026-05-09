#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_density;
uniform float u_size;
uniform float u_refract_str;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main() {
    vec2 uv = v_uv;
    vec2 grid = floor(uv * u_density);
    vec2 cell = fract(uv * u_density) - 0.5;
    // Each cell gets a random drop center and phase offset
    vec2 drop_center = vec2(hash(grid), hash(grid + vec2(7.3,3.1))) - 0.5;
    float drop_phase = hash(grid + vec2(13.7, 5.9));
    float anim = fract(u_time * 0.4 + drop_phase);
    float radius = u_size * 0.5 * smoothstep(0.0, 0.2, anim) * smoothstep(1.0, 0.7, anim);
    float dist = length(cell - drop_center * 0.3);
    if (dist < radius) {
        // Sphere lens refraction
        vec2 norm = (cell - drop_center * 0.3) / (radius + 0.001);
        float z = sqrt(max(0.0, 1.0 - dot(norm, norm)));
        vec2 refr = norm * (1.0 - z) * u_refract_str * 0.06;
        uv = v_uv - refr;
    }
    frag = texture(u_tex, clamp(uv, 0.0, 1.0));
}
