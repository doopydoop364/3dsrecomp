#version 450
// Triangles arrive already shaded, clipped and in screen space (the CPU vertex pipeline in pica.c).
// Attributes are pre-multiplied by 1/w, like the software rasteriser's, and interpolated linearly.
layout(location = 0) in vec4 a0;            // clip position (unused here)
layout(location = 1) in vec4 a1;            // quaternion
layout(location = 2) in vec4 a2;            // colour
layout(location = 3) in vec4 a3;            // tc0.xy tc1.xy
layout(location = 4) in vec4 a4;            // tc0.w, -, view.xy
layout(location = 5) in vec4 a5;            // view.z, -, tc2.xy
layout(location = 6) in float invw;
layout(location = 7) in ivec2 xy;           // 1/16 pixel units
layout(location = 8) in float z;

layout(push_constant) uniform PC { vec2 size; } pc;

layout(location = 0) noperspective out vec4 o1;
layout(location = 1) noperspective out vec4 o2;
layout(location = 2) noperspective out vec4 o3;
layout(location = 3) noperspective out vec4 o4;
layout(location = 4) noperspective out vec4 o5;
layout(location = 5) noperspective out vec2 oz;  // invw, z

void main() {
    vec2 p = vec2(xy) / 16.0 / pc.size * 2.0 - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);
    o1 = a1; o2 = a2; o3 = a3; o4 = a4; o5 = a5; oz = vec2(invw, z);
}
