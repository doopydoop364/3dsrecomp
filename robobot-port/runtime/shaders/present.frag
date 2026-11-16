#version 450
// mode 0: a native screen image (already upright); 1: a scaled render surface as the game left it (rows = PICA y,
// 240 * scale wide), turned upright the way the LCD shows it; 2: the overlay (menu, FPS) with alpha.
// info.z = 1 applies FXAA (mode 1 only).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { vec4 rect; vec4 info; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

vec2 surf_uv(vec2 s) {                     // screen position (0..1, x right, y down) -> surface texture coordinates
    float row = pc.info.y > 0.5 ? 1.0 - s.x : s.x;
    return vec2(1.0 - s.y, 1.0 - row);
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 fxaa(vec2 t) {                        // FXAA 3.11-style edge blend on the surface texels
    vec2 px = 1.0 / vec2(textureSize(src, 0));
    vec3 cM = texture(src, t).rgb;
    float lM = luma(cM), lN = luma(texture(src, t + vec2(0, -px.y)).rgb), lS = luma(texture(src, t + vec2(0, px.y)).rgb);
    float lW = luma(texture(src, t + vec2(-px.x, 0)).rgb), lE = luma(texture(src, t + vec2(px.x, 0)).rgb);
    float lNW = luma(texture(src, t - px).rgb), lSE = luma(texture(src, t + px).rgb);
    float lNE = luma(texture(src, t + vec2(px.x, -px.y)).rgb), lSW = luma(texture(src, t + vec2(-px.x, px.y)).rgb);
    float lMin = min(lM, min(min(lN, lS), min(lW, lE))), lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return cM;
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float red = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * px;
    vec3 a = 0.5 * (texture(src, t + dir * (1.0 / 3.0 - 0.5)).rgb + texture(src, t + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(src, t - dir * 0.5).rgb + texture(src, t + dir * 0.5).rgb);
    float lB = luma(b);
    return (lB < lMin || lB > lMax) ? a : b;
}
void main() {
    int mode = int(pc.info.x);
    if (mode == 1) {
        vec2 t = surf_uv(uv);
        o = vec4(pc.info.z > 0.5 ? fxaa(t) : texture(src, t).rgb, 1.0);
    } else if (mode == 2) o = texture(src, uv);
    else o = vec4(texture(src, uv).rgb, 1.0);
}
