#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_intensity;
uniform float u_speed;
uniform float u_height;
uniform float u_time;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
float noise2(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm3(vec2 p) {
    return noise2(p)*0.5 + noise2(p*2.1)*0.25 + noise2(p*4.3)*0.125;
}
void main() {
    vec4 col = texture(u_tex, v_uv);
    float t = u_time * u_speed;
    // Distance from nearest edge (0 = at edge, 1 = at center)
    float dist = min(min(v_uv.x, 1.0-v_uv.x), min(v_uv.y, 1.0-v_uv.y));
    // Fire rises inward from all edges
    float n = fbm3(vec2(v_uv.x * 5.0 + t * 0.3, v_uv.y * 5.0 - t * 0.6));
    float edge_fire = 1.0 - clamp(dist / (u_height * (0.5 + n * 0.5)), 0.0, 1.0);
    float flame = edge_fire * edge_fire * (n * 0.5 + 0.5);
    // Fire palette: black → red → orange → yellow → white
    float f = clamp(flame * u_intensity, 0.0, 1.0);
    vec3 fire_col;
    if (f < 0.25)      fire_col = mix(vec3(0.0), vec3(0.8, 0.1, 0.0), f*4.0);
    else if (f < 0.5)  fire_col = mix(vec3(0.8,0.1,0.0), vec3(1.0,0.5,0.05), (f-0.25)*4.0);
    else if (f < 0.75) fire_col = mix(vec3(1.0,0.5,0.05), vec3(1.0,0.9,0.3), (f-0.5)*4.0);
    else               fire_col = mix(vec3(1.0,0.9,0.3), vec3(1.0,1.0,0.9), (f-0.75)*4.0);
    float alpha = clamp(flame * u_intensity * 1.5, 0.0, 1.0);
    vec3 result = mix(col.rgb, max(col.rgb, fire_col), alpha);
    frag = vec4(clamp(result, 0.0, 1.0), col.a);
}
