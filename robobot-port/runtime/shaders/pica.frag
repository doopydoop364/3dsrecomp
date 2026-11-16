#version 450
// PICA200 per-fragment work, ported from the software rasteriser in pica.c (fragment(), sample(),
// combine(), lighting()) so the two renderers agree. Fixed-function blending, depth and stencil
// run in the Vulkan pipeline. Registers come raw in R; light parameters are decoded on the CPU.
layout(location = 0) noperspective in vec4 i1;   // quaternion
layout(location = 1) noperspective in vec4 i2;   // colour
layout(location = 2) noperspective in vec4 i3;   // tc0.xy tc1.xy
layout(location = 3) noperspective in vec4 i4;   // tc0.w, -, view.xy
layout(location = 4) noperspective in vec4 i5;   // view.z, -, tc2.xy
layout(location = 5) noperspective in vec2 iz;   // invw, z

layout(std140, set = 0, binding = 0) uniform Draw {
    uvec4 R[192];          // PICA registers 0x000-0x2ff
    vec4 L[48];            // 8 lights x 6: pos+dbias, spot+dscale, spec0+id, spec1+cfg, diff, amb
    vec4 gamb;             // global ambient
    vec4 misc;             // zscale, zoff, -, -
    uvec4 flags;           // x: flat mode, y: flat colour, z: light count, w: texture units on (bits)
    uvec4 texinfo[3];      // width, height, wrap_s | wrap_t << 8 | filtered << 16, border colour
} D;
layout(std430, set = 0, binding = 1) readonly buffer Lut { uint lut[24 * 256 + 128]; };   // lighting, then fog
layout(set = 0, binding = 2) uniform sampler2D tex0;
layout(set = 0, binding = 3) uniform sampler2D tex1;
layout(set = 0, binding = 4) uniform sampler2D tex2;

layout(location = 0) out vec4 frag;

uint reg(uint i) { return D.R[i >> 2][i & 3u]; }

bool cmp(uint f, int a, int b) {
    switch (f) { case 0u: return false; case 1u: return true; case 2u: return a == b; case 3u: return a != b;
                 case 4u: return a < b; case 5u: return a <= b; case 6u: return a > b; default: return a >= b; }
}

int wrapc(int v, int size, uint mode) {
    switch (mode) {
    case 0u: return clamp(v, 0, size - 1);
    case 3u: { uint c = uint(v) % uint(2 * size); return int(c >= uint(size) ? uint(2 * size) - 1u - c : c); }
    case 4u: return v < 0 ? int(uint(v) % uint(size)) : (v >= size ? size - 1 : v);
    default: return int(uint(v) % uint(size));
    }
}
// Filtered path (the game's filters / mip levels through a Vulkan sampler). Border modes are handled here;
// image row 0 holds the top texel row (t = height-1), so v is flipped.
ivec4 sampleFiltered(int u, vec2 uv, vec2 dx, vec2 dy) {
    uvec4 ti = D.texinfo[u];
    uint ws = ti.z & 7u, wt = (ti.z >> 8) & 7u;
    bool border = (ws == 1u && (uv.x < 0.0 || uv.x > 1.0)) || (wt == 1u && (uv.y < 0.0 || uv.y > 1.0)) || (ws == 5u && uv.x > 1.0) || (wt == 5u && uv.y > 1.0);
    if (border) return ivec4(ti.w & 255u, (ti.w >> 8) & 255u, (ti.w >> 16) & 255u, ti.w >> 24);
    vec2 c = vec2(uv.x, 1.0 - uv.y), f = vec2(1.0, -1.0);
    vec4 r = u == 0 ? textureGrad(tex0, c, dx * f, dy * f) : u == 1 ? textureGrad(tex1, c, dx * f, dy * f) : textureGrad(tex2, c, dx * f, dy * f);
    return ivec4(round(r * 255.0));
}
ivec4 sampleTex(int u, vec2 uv) {
    uvec4 ti = D.texinfo[u];
    int w = int(ti.x), h = int(ti.y);
    uint ws = ti.z & 7u, wt = (ti.z >> 8) & 7u;
    int s = int(uv.x * float(w)), t = int(uv.y * float(h));
    bool border = (ws == 1u && (s < 0 || s >= w)) || (wt == 1u && (t < 0 || t >= h)) || (ws == 5u && s >= w) || (wt == 5u && t >= h);
    if (border) return ivec4(ti.w & 255u, (ti.w >> 8) & 255u, (ti.w >> 16) & 255u, ti.w >> 24);
    ivec2 p = ivec2(wrapc(s, w, ws), h - 1 - wrapc(t, h, wt));
    vec4 c = u == 0 ? texelFetch(tex0, p, 0) : u == 1 ? texelFetch(tex1, p, 0) : texelFetch(tex2, p, 0);
    return ivec4(round(c * 255.0));
}

// ---- texture combiner ----
ivec3 tevOperand(uint m, ivec4 s) {
    switch (m) {
    case 0u: return s.rgb; case 1u: return 255 - s.rgb; case 2u: return ivec3(s.a); case 3u: return ivec3(255 - s.a);
    case 4u: return ivec3(s.r); case 5u: return ivec3(255 - s.r); case 8u: return ivec3(s.g); case 9u: return ivec3(255 - s.g);
    case 12u: return ivec3(s.b); case 13u: return ivec3(255 - s.b); default: return s.rgb;
    }
}
int tevAlphaOperand(uint m, ivec4 s) {
    uint c = (m & 7u) >> 1;                               // 0 a, 1 r, 2 g, 3 b
    int v = c == 0u ? s.a : c == 1u ? s.r : c == 2u ? s.g : s.b;
    return (m & 1u) != 0u ? 255 - v : v;
}
int tevOp(uint op, int a, int b, int c) {
    switch (op) {
    case 0u: return a;
    case 1u: return a * b / 255;
    case 2u: return min(a + b, 255);
    case 3u: return clamp(a + b - 128, 0, 255);
    case 4u: return (a * c + b * (255 - c)) / 255;
    case 5u: return max(a - b, 0);
    case 8u: return min((a * b + 255 * c) / 255, 255);
    case 9u: return min(a + b, 255) * c / 255;
    default: return 0;
    }
}
ivec4 combine(ivec4 vcol, ivec4 t0, ivec4 t1, ivec4 t2, ivec4 prim, ivec4 sec) {
    const uint tr[6] = uint[6](0x0c0u, 0x0c8u, 0x0d0u, 0x0d8u, 0x0f0u, 0x0f8u);
    uint bc = reg(0x0fdu), upd = reg(0x0e0u);
    ivec4 prev = ivec4(0), buf = ivec4(0), next = ivec4(bc & 255u, (bc >> 8) & 255u, (bc >> 16) & 255u, bc >> 24);
    for (int i = 0; i < 6; i++) {
        uint b = tr[i], src = reg(b), opd = reg(b + 1u), ops = reg(b + 2u), k = reg(b + 3u), sc = reg(b + 4u);
        uint cop = ops & 15u, aop = (ops >> 16) & 15u;
        int cscale = (sc & 3u) < 3u ? 1 << (sc & 3u) : 1, ascale = ((sc >> 16) & 3u) < 3u ? 1 << ((sc >> 16) & 3u) : 1;
        uint cs[3], as_[3], cm[3], am[3];
        for (int j = 0; j < 3; j++) {
            cs[j] = (src >> (4 * j)) & 15u; as_[j] = (src >> (16 + 4 * j)) & 15u;
            if (i == 0 && cs[j] == 15u) cs[j] = (src >> 8) & 15u;
            cm[j] = (opd >> (4 * j)) & 15u; am[j] = (opd >> (12 + 4 * j)) & 7u;
        }
        bool pass = i > 0 && cop == 0u && aop == 0u && cs[0] == 15u && as_[0] == 15u && cm[0] == 0u && am[0] == 0u && cscale == 1 && ascale == 1;
        if (!pass) {
            ivec4 kc = ivec4(k & 255u, (k >> 8) & 255u, (k >> 16) & 255u, k >> 24);
            ivec3 c[3]; int a[3];
            for (int j = 0; j < 3; j++) {
                ivec4 sv, sa;
                uint n = cs[j];
                sv = n == 0u ? vcol : n == 1u ? prim : n == 2u ? sec : n == 3u ? t0 : n == 4u ? t1 : n == 5u ? t2 : n == 6u ? ivec4(0) : n == 13u ? buf : n == 14u ? kc : prev;
                n = as_[j];
                sa = n == 0u ? vcol : n == 1u ? prim : n == 2u ? sec : n == 3u ? t0 : n == 4u ? t1 : n == 5u ? t2 : n == 6u ? ivec4(0) : n == 13u ? buf : n == 14u ? kc : prev;
                c[j] = tevOperand(cm[j], sv);
                a[j] = tevAlphaOperand(am[j], sa);
            }
            ivec4 res;
            if (cop == 6u || cop == 7u) {
                int d = 0;
                for (int ch = 0; ch < 3; ch++) d += ((2 * c[0][ch] - 255) * (2 * c[1][ch] - 255) + 128) / 256;
                d = clamp(d, 0, 255);
                res.rgb = ivec3(d);
            } else for (int ch = 0; ch < 3; ch++) res[ch] = tevOp(cop, c[0][ch], c[1][ch], c[2][ch]);
            res.a = cop == 7u ? res.r : tevOp(aop, a[0], a[1], a[2]);
            if (cscale != 1) res.rgb = min(res.rgb * cscale, 255);
            if (ascale != 1) res.a = min(res.a * ascale, 255);
            prev = res;
        }
        buf = next;
        if (((upd >> (8 + i)) & 1u) != 0u) next.rgb = prev.rgb;
        if (((upd >> (12 + i)) & 1u) != 0u) next.a = prev.a;
    }
    return prev;
}

// ---- fragment lighting ----
float lutEntry(uint type, uint idx, float delta) {
    uint e = lut[type * 256u + (idx & 255u)];
    float value = float(e & 0xfffu) / 4095.0, diff = float((e >> 12) & 0x7ffu) / 2047.0;
    if ((e & (1u << 23)) != 0u) diff = -diff;
    return value + diff * delta;
}
bool lutSupported(uint id, uint config) {
    switch (id) {
    case 0u: return config != 1u;
    case 1u: return config != 0u && config != 1u && config != 5u;
    case 2u: return config != 2u && config != 3u;
    case 3u: return config != 0u && config != 2u && config != 4u;
    case 6u: return config != 3u;
    default: return config == 4u || config == 5u || config == 8u;
    }
}
float lutScale(uint s) { const float t[8] = float[8](1.0, 2.0, 4.0, 8.0, 0.0, 0.0, 0.25, 0.5); return t[s & 7u]; }
vec3 qrot(vec4 q, vec3 v) { vec3 t = cross(q.xyz, v) + v * q.w; return v + 2.0 * cross(q.xyz, t); }
vec3 safeNorm(vec3 v) { float l = sqrt(dot(v, v)); return l > 0.0 ? v / l : v; }

float in_val[6];
uint g_insel, g_absin, g_scl;
bool g_two;
float sampleLut(uint lut_id, uint type) {
    uint sh = lut_id * 4u;
    float x = in_val[((g_insel >> sh) & 7u) % 6u], r;
    if (((g_absin >> (sh + 1u)) & 1u) == 0u) {
        x = g_two ? abs(x) : max(x, 0.0);
        int i = clamp(int(floor(x * 256.0)), 0, 255);
        r = lutEntry(type, uint(i), x * 256.0 - float(i));
    } else {
        int i = clamp(int(floor(x * 128.0)), -128, 127);
        r = lutEntry(type, uint(i) & 255u, x * 128.0 - float(i));
    }
    return r * lutScale((g_scl >> sh) & 7u);
}

void lighting(vec4 qin, vec3 view, ivec4 t0, ivec4 t1, ivec4 t2, out ivec4 prim, out ivec4 sec) {
    uint c0 = reg(0x1c3u), c1 = reg(0x1c4u), config = (c0 >> 4) & 15u;
    g_absin = reg(0x1d0u); g_insel = reg(0x1d1u); g_scl = reg(0x1d2u);
    vec4 q = qin; float ql = sqrt(dot(q, q)); if (ql > 0.0) q /= ql;
    ivec4 tx[3] = ivec4[3](t0, t1, t2);
    vec4 shadow = vec4(1.0);
    if ((c0 & 1u) != 0u) {
        uint u = (c0 >> 24) & 3u; vec4 v = u < 3u ? vec4(tx[u]) / 255.0 : vec4(0.0);
        shadow = ((c0 >> 18) & 1u) != 0u ? 1.0 - v : v;
    }
    vec3 ns = vec3(0, 0, 1), ts = vec3(1, 0, 0);
    uint bump = (c0 >> 28) & 3u;
    if (bump == 1u || bump == 2u) {
        uint u = (c0 >> 22) & 3u; vec4 tv = u < 3u ? vec4(tx[u]) : vec4(0.0);
        vec3 p = tv.rgb / 127.5 - 1.0;
        if (bump == 1u) {
            if (((c0 >> 30) & 1u) == 0u) { float zz = 1.0 - (p.x * p.x + p.y * p.y); p.z = sqrt(max(zz, 0.0)); }
            ns = p;
        } else ts = p;
    } else if (bump == 3u) { ns = vec3(0); ts = vec3(0); }
    vec3 n = qrot(q, ns), tg = qrot(q, ts);
    vec3 V = safeNorm(view);
    vec4 dsum = vec4(0, 0, 0, 1), ssum = vec4(0, 0, 0, 1);
    uint nl = D.flags.z;
    for (uint k = 0u; k < nl; k++) {
        vec4 P0 = D.L[k * 6u], P1 = D.L[k * 6u + 1u], P2 = D.L[k * 6u + 2u], P3 = D.L[k * 6u + 3u], P4 = D.L[k * 6u + 4u], P5 = D.L[k * 6u + 5u];
        uint id = floatBitsToUint(P2.w), cfg = floatBitsToUint(P3.w);
        g_two = ((cfg >> 1) & 1u) != 0u;
        vec3 Lv = P0.xyz;
        if ((cfg & 1u) == 0u) Lv += view;
        float len = sqrt(dot(Lv, Lv));
        Lv = safeNorm(Lv);
        vec3 H = V + Lv;
        float hlen2 = dot(H, H);
        vec3 Hn = safeNorm(H);
        in_val[0] = dot(n, Hn); in_val[1] = dot(V, Hn); in_val[2] = dot(n, V); in_val[3] = dot(Lv, n); in_val[4] = dot(Lv, P1.xyz);
        { float nh = dot(n, Hn); vec3 p = Hn - n * nh; in_val[5] = dot(p, tg); }
        float spot_att = (((c1 >> (8u + id)) & 1u) == 0u && lutSupported(2u, config)) ? sampleLut(2u, 8u + id) : 1.0;
        float d0 = (((c1 >> 16) & 1u) == 0u && lutSupported(0u, config)) ? sampleLut(0u, 0u) : 1.0;
        vec3 refl;
        refl.r = (((c1 >> 20) & 1u) == 0u && lutSupported(6u, config)) ? sampleLut(6u, 6u) : 1.0;
        refl.g = (((c1 >> 21) & 1u) == 0u && lutSupported(5u, config)) ? sampleLut(5u, 5u) : refl.r;
        refl.b = (((c1 >> 22) & 1u) == 0u && lutSupported(4u, config)) ? sampleLut(4u, 4u) : refl.r;
        float d1 = (((c1 >> 17) & 1u) == 0u && lutSupported(1u, config)) ? sampleLut(1u, 1u) : 1.0;
        if (k == nl - 1u && ((c1 >> 19) & 1u) == 0u && lutSupported(3u, config)) {
            float fr = sampleLut(3u, 3u);
            if (((c0 >> 2) & 1u) != 0u) dsum.a = fr;
            if (((c0 >> 3) & 1u) != 0u) ssum.a = fr;
        }
        float dist = 1.0;
        if (((c1 >> (24u + id)) & 1u) == 0u) {
            float s = clamp(P1.w * len + P0.w, 0.0, 1.0);
            int i = clamp(int(floor(s * 256.0)), 0, 255);
            dist = lutEntry(16u + id, uint(i), s * 256.0 - float(i));
        }
        float ndl = g_two ? abs(in_val[3]) : max(in_val[3], 0.0);
        float clamp_hl = (((c0 >> 27) & 1u) != 0u && ndl == 0.0) ? 0.0 : 1.0;
        float geo = hlen2 == 0.0 ? 0.0 : min(ndl / hlen2, 1.0);
        bool sp_on = ((c0 >> 16) & 1u) != 0u && ((c1 >> id) & 1u) == 0u, ss_on = ((c0 >> 17) & 1u) != 0u && ((c1 >> id) & 1u) == 0u;
        for (int ch = 0; ch < 3; ch++) {
            float s0 = d0 * P2[ch], s1 = d1 * refl[ch] * P3[ch];
            if (((cfg >> 2) & 1u) != 0u) s0 *= geo;
            if (((cfg >> 3) & 1u) != 0u) s1 *= geo;
            float shp = sp_on ? shadow[ch] : 1.0, shs = ss_on ? shadow[ch] : 1.0;
            dsum[ch] += (P4[ch] * ndl * shp + P5[ch]) * dist * spot_att;
            ssum[ch] += (s0 + s1) * clamp_hl * dist * spot_att * shs;
        }
    }
    if (((c0 >> 19) & 1u) != 0u) {
        if (((c0 >> 2) & 1u) != 0u) dsum.a *= shadow.a;
        if (((c0 >> 3) & 1u) != 0u) ssum.a *= shadow.a;
    }
    dsum.rgb += D.gamb.rgb;
    prim = ivec4(clamp(dsum, 0.0, 1.0) * 255.0);
    sec = ivec4(clamp(ssum, 0.0, 1.0) * 255.0);
}

void main() {
    // texture coordinates and their screen derivatives, before anything can discard
    float iw0 = 1.0 / iz.x;
    vec2 uv0 = i3.xy * iw0;
    if (((reg(0x083u) >> 28) & 7u) == 3u) uv0 /= i4.x * iw0;
    vec2 uv1 = i3.zw * iw0, uv2 = (((reg(0x080u) >> 13) & 1u) != 0u ? i3.zw : i5.zw) * iw0;
    vec2 dx0 = dFdx(uv0), dy0 = dFdy(uv0), dx1 = dFdx(uv1), dy1 = dFdy(uv1), dx2 = dFdx(uv2), dy2 = dFdy(uv2);
    ivec2 px = ivec2(gl_FragCoord.xy);
    uint sc = reg(0x065u) & 3u;
    if (sc == 1u) {
        uint s1 = reg(0x066u), s2 = reg(0x067u);
        if (uint(px.x) >= (s1 & 0x3ffu) && uint(px.x) <= (s2 & 0x3ffu) && uint(px.y) >= ((s1 >> 16) & 0x3ffu) && uint(px.y) <= ((s2 >> 16) & 0x3ffu)) discard;
    }
    float iw = 1.0 / iz.x;
    float depth = iz.y * D.misc.x + D.misc.y;
    if ((reg(0x06du) & 1u) == 0u) depth *= iw;
    gl_FragDepth = clamp(depth, 0.0, 1.0);
    if (D.flags.x != 0u) { uint f = D.flags.y; frag = vec4(f & 255u, (f >> 8) & 255u, (f >> 16) & 255u, 255u) / 255.0; return; }

    ivec4 vcol = ivec4(clamp(roundEven(i2 * iw * 255.0), 0.0, 255.0));
    ivec4 t0 = ivec4(0), t1 = ivec4(0), t2 = ivec4(0);
    uint on = D.flags.w;
    if ((on & 1u) != 0u) t0 = ((D.texinfo[0].z >> 16) & 1u) != 0u ? sampleFiltered(0, uv0, dx0, dy0) : sampleTex(0, uv0);
    if ((on & 2u) != 0u) t1 = ((D.texinfo[1].z >> 16) & 1u) != 0u ? sampleFiltered(1, uv1, dx1, dy1) : sampleTex(1, uv1);
    if ((on & 4u) != 0u) t2 = ((D.texinfo[2].z >> 16) & 1u) != 0u ? sampleFiltered(2, uv2, dx2, dy2) : sampleTex(2, uv2);
    ivec4 prim = ivec4(0), sec = ivec4(0);
    if ((reg(0x1c6u) & 1u) == 0u) lighting(i1 * iw, vec3(i4.zw, i5.x) * iw, t0, t1, t2, prim, sec);
    ivec4 o = combine(vcol, t0, t1, t2, prim, sec);
    uint at = reg(0x104u);
    if ((at & 1u) != 0u && !cmp((at >> 4) & 7u, o.a, int((at >> 8) & 255u))) discard;
    uint fm = reg(0x0e0u);
    if ((fm & 7u) == 5u) {                                // fog
        float d = gl_FragDepth;
        float idx = ((fm >> 16) & 1u) != 0u ? (1.0 - d) * 128.0 : d * 128.0;
        float fi = clamp(floor(idx), 0.0, 127.0);
        uint e = lut[24u * 256u + uint(fi)];
        float f = clamp(float((e >> 13) & 0x7ffu) / 2047.0 + float(int(e << 19) >> 19) / 2047.0 * (idx - fi), 0.0, 1.0);
        uint fc = reg(0x0e1u);
        vec3 fcol = vec3(fc & 255u, (fc >> 8) & 255u, (fc >> 16) & 255u);
        o.rgb = ivec3(f * vec3(o.rgb) + (1.0 - f) * fcol);
    }
    frag = vec4(o) / 255.0;
}
