#version 450
// One textured quad per call (triangle strip of 4 vertices), placed by a rectangle in normalized device coordinates.
layout(push_constant) uniform P { vec4 rect; vec4 info; } pc;   // rect: x0 y0 x1 y1; info: mode, flip, texel w, texel h
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    uv = c;
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c), 0.0, 1.0);
}
