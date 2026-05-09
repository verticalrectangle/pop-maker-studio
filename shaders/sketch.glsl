#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_tex;
uniform float u_tex_w;
uniform float u_tex_h;
uniform float u_strength;
uniform float u_invert;
void main() {
    vec2 px = vec2(1.0 / u_tex_w, 1.0 / u_tex_h);
    // Sobel edge detection on luma
    float tl = dot(texture(u_tex, v_uv + vec2(-px.x,  px.y)).rgb, vec3(0.299,0.587,0.114));
    float tc = dot(texture(u_tex, v_uv + vec2( 0.0,   px.y)).rgb, vec3(0.299,0.587,0.114));
    float tr = dot(texture(u_tex, v_uv + vec2( px.x,  px.y)).rgb, vec3(0.299,0.587,0.114));
    float ml = dot(texture(u_tex, v_uv + vec2(-px.x,  0.0 )).rgb, vec3(0.299,0.587,0.114));
    float mr = dot(texture(u_tex, v_uv + vec2( px.x,  0.0 )).rgb, vec3(0.299,0.587,0.114));
    float bl = dot(texture(u_tex, v_uv + vec2(-px.x, -px.y)).rgb, vec3(0.299,0.587,0.114));
    float bc = dot(texture(u_tex, v_uv + vec2( 0.0,  -px.y)).rgb, vec3(0.299,0.587,0.114));
    float br = dot(texture(u_tex, v_uv + vec2( px.x, -px.y)).rgb, vec3(0.299,0.587,0.114));
    float gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    float gy = -tl - 2.0*tc - tr + bl + 2.0*bc + br;
    float edge = clamp(sqrt(gx*gx + gy*gy) * u_strength * 4.0, 0.0, 1.0);
    float sketch = (u_invert > 0.5) ? (1.0 - edge) : edge;
    vec4 orig = texture(u_tex, v_uv);
    frag = vec4(orig.rgb * sketch, orig.a);
}
