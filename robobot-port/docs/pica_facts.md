# PICA200 facts used by the renderer

Collected from the Azahar emulator sources (master, November 2026) and the nihstro shader header.
These are reference facts for an independent implementation, not code to copy. Register indices are
word indices in the 0x000-0x2FF internal register space. Bit ranges are inclusive, LSB = 0.

## Command lists
- Entries are [value, header] word pairs. Header: register id bits 0-15, byte-enable mask bits 16-19
  (bit i enables byte i), extra word count bits 20-27, consecutive bit 31.
- Extra words follow the header: consecutive=0 writes the same register again (shader uploads),
  consecutive=1 writes id+1, id+2, ... Each entry starts on an 8-byte boundary (pad odd word index).
- Write 0x22E = draw, 0x22F = indexed draw (value ignored). Write 0x23C/0x23D = jump to the nested
  list at reg 0x23A/0x23B *8 with size 0x238/0x239 *8.

## Register blocks
rasterizer 0x040, texturing 0x080, framebuffer 0x100 (output merger 0x100, config 0x110),
lighting 0x140, pipeline 0x200, geometry shader 0x280, vertex shader 0x2B0.

## Rasterizer
| Reg | Field |
|---|---|
| 0x040 bits 0-1 | cull: 0 keep all, 1 keep clockwise, 2 keep counter-clockwise, 3 keep all |
| 0x041 / 0x043 bits 0-23 | viewport half width / half height (float24) |
| 0x047 bit 0, 0x048-0x04B | user clip plane enable, coefficients (float24) |
| 0x04D / 0x04E bits 0-23 | depth range / near plane (float24) |
| 0x04F bits 0-2 | number of shader output attributes |
| 0x050-0x056 | output attribute i: semantic of x bits 0-4, y 8-12, z 16-20, w 24-28 |
| 0x065 bits 0-1 | scissor: 0 off, 1 exclude inside, 3 keep inside |
| 0x066 / 0x067 | scissor x1 bits 0-9 y1 16-25 / x2 0-9 y2 16-25 |
| 0x068 | viewport corner x bits 0-9, y bits 16-25 (signed) |
| 0x06D bit 0 | depth map: 0 W-buffer, 1 Z-buffer |

Semantics: POS x,y,z,w 0-3; QUAT 4-7; COLOR r,g,b,a 8-11; TEX0 u,v 12-13; TEX1 u,v 14-15;
TEX0 w 16; VIEW x,y,z 18-20; TEX2 u,v 22-23; unused 31. Unmapped slots default to 1.0. Colours are
clamped to min(|c|, 1).

## Framebuffer / output merger
| Reg | Field |
|---|---|
| 0x100 bits 0-1, bit 8 | mode (0 normal, 1 gas, 3 shadow); 1 = alpha blending, 0 = logic op |
| 0x101 | rgb equation 0-2, alpha equation 8-10 (add, sub, reverse sub, min, max); src rgb 16-19, dst rgb 20-23, src a 24-27, dst a 28-31 |
| 0x102 bits 0-3 | logic op (clear, and, and-reverse, copy, set, copy-inverted, noop, invert, nand, or, nor, xor, equiv, and-inverted, or-reverse, or-inverted) |
| 0x103 | blend constant r,g,b,a bytes |
| 0x104 | alpha test enable 0, func 4-6, ref 8-15 |
| 0x105 / 0x106 | stencil: enable 0, func 4-6, write mask 8-15, ref 16-23, input mask 24-31 / fail 0-2, zfail 4-6, zpass 8-10 |
| 0x107 | depth test enable 0, func 4-6; write r,g,b,a bits 8-11; depth write bit 12 |
| 0x113 / 0x115 | colour write allowed / depth-stencil write allowed (0 = off) |
| 0x116 bits 0-1 | depth format: 0 D16, 2 D24, 3 D24S8 |
| 0x117 bits 16-18 | colour format: 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565, 4 RGBA4 |
| 0x11C / 0x11D | depth / colour buffer physical address /8 |
| 0x11E | width bits 0-10; height-1 bits 12-21; bit 24 flip (flipped when the bit is 0) |

Blend factors: 0 zero, 1 one, 2 src colour, 3 1-src colour, 4 dst colour, 5 1-dst colour, 6 src alpha,
7 1-src alpha, 8 dst alpha, 9 1-dst alpha, 10 const colour, 11 1-const colour, 12 const alpha,
13 1-const alpha, 14 src alpha saturate. Compare: never, always, eq, ne, lt, le, gt, ge.
Stencil actions: keep, zero, replace, incr, decr, invert, incr-wrap, decr-wrap.

## Texturing
- 0x080: texture 0/1/2 enable bits 0-2; tex3 coords bits 8-9; tex3 enable bit 10; tex2 uses coords 1 bit 13.
- Units at 0x081 / 0x091 / 0x099: +0 border colour, +1 height bits 0-10 width 16-26, +2 mag filter
  bit 1, min filter bit 2, wrap t 8-10, wrap s 12-14, mip filter 24, type 28-30 (unit 0 only:
  2D, cube, shadow 2D, projection, shadow cube, disabled), +3 lod, +4 address /8.
- Format at 0x08E / 0x096 / 0x09E bits 0-3: RGBA8 0, RGB8 1, RGB5A1 2, RGB565 3, RGBA4 4, IA8 5, RG8 6,
  I8 7, A8 8, IA4 9, I4 10, A4 11, ETC1 12, ETC1A4 13.
- Wrap: clamp-edge 0, clamp-border 1, repeat 2, mirror 3. Filter: 0 nearest, 1 linear.

## Texture combiner (TEV)
- Stages at 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8. Each: +0 sources (rgb 1,2,3 in bits 0-3, 4-7, 8-11;
  alpha 16-19, 20-23, 24-27), +1 operands (rgb 0-3, 4-7, 8-11; alpha 12-14, 16-18, 20-22),
  +2 op rgb 0-3 op alpha 16-19, +3 constant colour, +4 scale rgb 0-1 alpha 16-17 (1x, 2x, 4x, 1x).
- Sources: 0 vertex colour, 1 primary fragment, 2 secondary fragment, 3-6 texture 0-3,
  0xD combiner buffer, 0xE constant, 0xF previous stage.
- Colour operands: 0 c, 1 1-c, 2 a, 3 1-a, 4 r, 5 1-r, 8 g, 9 1-g, 0xC b, 0xD 1-b.
  Alpha operands: 0 a, 1 1-a, 2 r, 3 1-r, 4 g, 5 1-g, 6 b, 7 1-b.
- Ops: 0 replace, 1 modulate, 2 add, 3 add signed, 4 lerp, 5 subtract, 6 dot3 rgb, 7 dot3 rgba,
  8 multiply-add, 9 add-multiply.
- 0x0E0: fog mode bits 0-2; buffer update masks rgb bits 8-11, alpha bits 12-15 (bit i = stage i
  writes the buffer); 0x0FD initial buffer colour.

## Vertex input (pipeline)
- 0x200: attribute base, physical = field (bits 1-28) * 16, i.e. register value * 8.
- 0x201/0x202: per attribute 4 bits: type bits 0-1 (byte, ubyte, short, float), count-1 bits 2-3.
  0x202 bits 16-27: attribute uses its fixed value; bits 28-31: attribute count - 1.
- Loaders i=0..11 at 0x203+3i: +0 offset, +1 components 0-7 (4 bits each), +2 components 8-11 in
  bits 0-15, byte stride bits 16-23, component count bits 28-31.
- 0x227 index buffer offset bits 0-27 (from attribute base), bit 31: 16-bit indices.
- 0x228 vertex count, 0x22A first vertex (non-indexed only). 0x25E bits 8-9 topology: list, strip,
  fan, shader. Write 0x25F restarts the primitive. 0x232 fixed attribute index, 0x233-0x235 its value.
- Vertex cache of 64 entries keyed by index (indexed draws only).

## Vertex shader
- Registers at 0x2B0: bools +0, ints +1..+4 (x,y,z,w bytes), input count-1 +9 bits 0-3, entry point
  +0xA, input register map +0xB/+0xC (4 bits per attribute), output mask +0xD, uniform index +0x10
  (bits 0-6, bit 31 float32 mode) and data +0x11..+0x18, program offset +0x1B and data +0x1C..+0x23,
  operand descriptor offset +0x25 and data +0x26..+0x2D.
- Uniform packing, float24 mode, 3 words b0,b1,b2: x = b2 & 0xFFFFFF; y = (b1 & 0xFFFF) << 8 | b2 >> 24;
  z = (b0 & 0xFF) << 16 | b1 >> 16; w = b0 >> 8. Float32 mode: 4 words w, z, y, x.
- float24: sign bit 23, exponent bits 16-22 (bias 63), mantissa 0-15. Exponent 0 is zero (keep
  denormals), 0x7F is inf/NaN. Computation is done in float32.
- Outputs: output mask bit order assigns o-registers to output attribute slots 0, 1, 2, ...

### Instruction encoding
opcode bits 26-31. Arithmetic: descriptor 0-6; src2 7-11 (5 bit) or inverted src2 7-13 (7 bit); src1
12-18 (7 bit) or inverted 14-18 (5 bit); address index 19-20; dest 21-25. CMP: op y 21-23, op x 24-26.
MAD: descriptor 0-4; src3 5-9 (MADI 5-11); src2 10-16 (MADI 12-16); src1 17-21; address 22-23;
dest 24-28. Flow: count 0-7, target 10-21, condition 22-23 (or, and, x, y), bool id 22-25,
int id 22-23, refy 24, refx 25.

Opcodes: 00 ADD, 01 DP3, 02 DP4, 03 DPH, 04 DST, 05 EX2, 06 LG2, 07 LIT, 08 MUL, 09 SGE, 0A SLT,
0B FLR, 0C MAX, 0D MIN, 0E RCP, 0F RSQ, 12 MOVA, 13 MOV, 18 DPHI, 19 DSTI, 1A SGEI, 1B SLTI,
20 BREAK, 21 NOP, 22 END, 23 BREAKC, 24 CALL, 25 CALLC, 26 CALLU, 27 IFU, 28 IFC, 29 LOOP, 2A EMIT,
2B SETEMIT, 2C JMPC, 2D JMPU, 2E-2F CMP, 30-37 MADI, 38-3F MAD.

Registers: source 0x00-0x0F inputs, 0x10-0x1F temporaries, 0x20-0x7F uniforms c0-c95 (7-bit fields
only). Destination 0x00-0x0F outputs, 0x10-0x1F temporaries. Relative addressing (1 a0.x, 2 a0.y,
3 aL) applies to the 7-bit source and only to uniforms; index = (index + offset) & 0x7F, >= 96 gives 1.0.

Operand descriptor: dest mask bits 0-3 (bit 3 = x); src1 negate 4, swizzle 5-12; src2 negate 13,
swizzle 14-21; src3 negate 22, swizzle 23-30. Each swizzle has x in the top 2 bits (x=0..w=3).

Semantics: MUL gives 0 for 0*inf. DP3/DP4/DPH write the dot to every enabled component (DPH uses
w=1 for src1). RCP, RSQ, EX2, LG2 use the x component and write all enabled components. SGE/SLT give
1.0 or 0.0. MAX = src1 > src2 ? src1 : src2, MIN = src1 < src2 ? src1 : src2. MOVA truncates x,y into
a0.x, a0.y. CMP sets two flags from x and y (eq, ne, lt, le, gt, ge). JMPU jumps when
bool == !(count & 1). CALL: return after target+count-1, stack depth 4. IF: else at target, end at
target+count, depth 8. LOOP: aL = int.y, step int.z, runs int.x+1 times, body ends at target
(inclusive), depth 4. END stops.

## Vertex loading
- Loader component codes: 0-11 = attribute index (align offset to the element size, then add
  count*size); 12-15 = padding of 4, 8, 12, 16 bytes (align to 4 first). Attribute address =
  base + loader offset + running offset + vertex * loader byte stride.
- Values are converted as-is (no normalisation): byte signed, ubyte unsigned, short signed.
  Missing components: w = 1, others 0. Attributes >= 12 or with their bit in the mask use the
  fixed value set through 0x232-0x235 (float24, 3-word packing as for uniforms).
- Indexed: vertex = index (8 or 16 bit); non-indexed: vertex = i + first vertex.

## Primitive assembly
List: (v0,v1,v2), (v3,v4,v5)... Strip: triangle k uses (vk, vk+1, vk+2) and swaps the first two
for odd k. Fan: (v0, vk+1, vk+2).

## Clipping and viewport
- Clip in homogeneous space against x<=w, x>=-w, y<=w, y>=-w, z<=0, z>=-w, w>=~0 (epsilon), then
  the user plane if enabled; fan-triangulate the polygon. Attributes are lerped in clip space.
- Screen: x = (x/w + 1) * halfwidth + corner.x, y = (y/w + 1) * halfheight + corner.y, z = z/w.
  No y flip here. All attributes are multiplied by 1/w for perspective-correct interpolation.
- Depth: Z-buffer mode depth = z/w * range + near; W-buffer mode multiplies that by the
  perspective-correct w. Clamp to [0, 1]. Stored as truncated depth * (2^16-1) or (2^24-1).
- Culling in screen space on 12.4 fixed point, area = cross(b-a, c-a): mode 2 keeps area > 0,
  mode 1 keeps area < 0, modes 0/3 keep both (reorder if negative). Zero area is culled in 1/2.

## Rasteriser
- 12.4 fixed point, sample at pixel centre (+0.5), covered when all three edge functions >= 0, with
  a -1 bias for right-side or flat-bottom edges (top-left rule).
- Scissor bounds are inclusive (x1..x2). Attribute interpolation is perspective-correct.
- Vertex colour to 8 bits with rounding. Lighting off: primary and secondary fragment colour = 0.
- Pixel order: alpha test, fog, depth/stencil, colour write (only if 0x113 allows it).
- Stencil only with D24S8; compares (ref & mask) against (stored & mask). Depth write happens when
  allowed and the write bit is set, even if the depth test is disabled.
- Blending: per channel src*srcfactor and dst*dstfactor (0..255 factors), then add / sub / rev-sub /
  min / max of those products, divided by 255, clamped. Min/max do NOT ignore the factors.
  Source-alpha-saturate: rgb factor min(src.a, 255-dst.a), alpha factor 255. Write masks last.

## Framebuffer memory
- Pixel (x, y): y' = height_reg - y (height_reg holds height-1, so the image is stored bottom-up),
  offset = morton(x, y') + (x & ~7) * 8 texels within the tile row + (y' & ~7) * width, times bpp.
  Morton within 8x8: x bits interleave as x0,y0,x1,y1,x2,y2 (x lut 0,1,4,5,16,17,20,21;
  y lut 0,2,8,10,32,34,40,42).
- Colour: RGBA8 bytes A,B,G,R; RGB8 B,G,R; 16-bit formats little-endian with R in the top bits.
  Depth: D16 u16; D24 3 bytes LE; D24S8 3 bytes depth + stencil in byte 3.

## Textures
- Texel (s, t): t' = height-1-wrapped t; 8x8 tiles in rows, tile size = 64 texels * bpp
  (ETC1 32 bytes, ETC1A4 64); texel = tile + morton(x%8, y%8) * bpp.
- Coordinates: s = (int)(u * width), t = (int)(v * height), no half-texel offset. The reference
  software renderer uses nearest sampling only.
- Wrap: clamp-edge clamp(0, size-1); clamp-border uses the border colour outside [0, size);
  repeat (u32)val % size; mirror c = val % 2size, c >= size -> 2size-1-c.
- Formats: IA8 bytes A,I; RG8 bytes G,R (B=0, A=255); I8; A8 (rgb 0); IA4 high nibble I, low A;
  I4/A4 even morton index = low nibble.
- ETC1: an 8x8 tile is four 4x4 blocks, block index (x/4) + 2*(y/4); ETC1A4 puts a 64-bit alpha
  block (nibble at 4*x+y) before each 64-bit colour block. Colour block u64 LE: index bits 0-15 and
  sign bits 16-31 at texel 4*x+y; flip bit 32; differential bit 33; tables 34-36 (second half)
  and 37-39 (first half); base colours in bits 40-63. Modifier table {2,8} {5,17} {9,29} {13,42}
  {18,60} {24,80} {33,106} {47,183}.

## Texture combiner evaluation (8-bit)
Replace a; modulate a*b/255; add min(255,a+b); add-signed clamp(a+b-128); lerp (a*c + b*(255-c))/255;
subtract max(0,a-b); dot3 sum over rgb of ((2x-255)(2y-255)+128)/256 clamped, replicated (RGBA also to
alpha); multiply-add min(255,(a*b+255c)/255); add-multiply min(255,a+b)*c/255. Scale 1, 2, 4 (3 -> 1),
then clamp. Stage 0 with source "previous" for colour uses source 3 instead. The combiner buffer
starts at reg 0x0FD; the reference updates it with a one-stage delay (noted as possibly inaccurate).

## Copied from the reference, possibly not hardware-accurate
These follow Azahar's software renderer on purpose; revisit them if output differs from a console.
- Texture sampling is nearest only (filters and mipmaps ignored).
- The combiner buffer lags one stage behind its update masks.
- In stage 0, a colour source of "previous" is replaced by the stage's third colour source.
- Fragment lighting is not implemented at all here. TEMPORARY in runtime/pica.c: lit draws get a
  white primary fragment colour so lit surfaces are not black. Fog: implemented since; see below.

## Learned from the replay test
- Command-list chains can be long: Robobot's 3D scene jumps (0x23C/0x23D) about 174 times per pass,
  alternating between FCRAM and VRAM lists. Never cap jumps low.

## Fragment lighting (Azahar software renderer)
- Lights at 0x140 + 0x10*i: +0 specular0, +1 specular1, +2 diffuse, +3 ambient (b bits 0-9, g 10-19, r 20-29, /255);
  +4 x (f16 bits 0-15), y (f16 bits 16-31); +5 z (f16); +6 spot x (s13 bits 0-12), y (bits 16-28); +7 spot z;
  +9 config: bit 0 directional, 1 two-sided diffuse, 2 geometric factor 0, 3 geometric factor 1;
  +0xA distance attenuation bias (f20), +0xB scale (f20).
- 0x1C0 global ambient; 0x1C2 bits 0-2 lights-1; 0x1C3 config0 (0 shadow, 2 fresnel to primary alpha, 3 to secondary
  alpha, 4-7 config, 16 shadow primary, 17 shadow secondary, 18 invert shadow, 19 shadow alpha, 22-23 bump texture,
  24-25 shadow texture, 27 clamp highlights, 28-29 bump mode, 30 no bump renormalisation); 0x1C4 config1 (0-7 no
  shadow per light, 8-15 no spot attenuation, 16 no D0, 17 no D1, 19 no FR, 20 no RR, 21 no RG, 22 no RB, 24-31 no
  distance attenuation); 0x1C5 LUT index bits 0-7, type bits 8-12; 0x1C6 bit 0 = lighting disabled (the reference
  software renderer gates lighting only on this, not on 0x08F); 0x1C8-0x1CF LUT data (index auto-increments);
  0x1D0 per-LUT "signed input" bits (D0 1, D1 5, SP 9, FR 13, RB 17, RG 21, RR 25); 0x1D1 input selector and
  0x1D2 scale, 3 bits at D0 0, D1 4, SP 8, FR 12, RB 16, RG 20, RR 24; 0x1D9 light slot k -> light id bits 4k..4k+2.
- Inputs NH 0, VH 1, NV 2, LN 3, SP 4, CP 5. Scales 0 1x, 1 2x, 2 4x, 3 8x, 6 0.25x, 7 0.5x. LUT types D0 0, D1 1,
  FR 3, RB 4, RG 5, RR 6, SP0-7 8-15, DA0-7 16-23. Configs 0-6, Config7 = 8. Availability: D0 all but 1; D1 not 0,1,5;
  SP not 2,3; FR not 0,2,4; RR not 3; RG/RB only 4,5,7.
- LUT entry: value bits 0-11 /4095, difference bits 12-22 /2047, sign bit 23. Unsigned input: x = two-sided ? |x| :
  max(x,0), idx = clamp(floor(256x),0,255), d = 256x-idx. Signed input: i = clamp(floor(128x),-128,127), d = 128x-i,
  index = (u8)i. Result (value + diff*d) * scale. Distance LUT: s = clamp(scale*len+bias,0,1), same unsigned index, no scale.
- f16: 10-bit mantissa, 5-bit exponent (bias 15), sign bit 15. f20: 12-bit mantissa, 7-bit exponent (bias 63), sign bit 19.
- Normal: quaternion interpolated perspective-correct then normalised; rotate (0,0,1) (or the bump-map normal);
  rotate(q,v) = v + 2*cross(q.xyz, cross(q.xyz,v) + q.w*v). Before clipping, vertex 1 and 2 quaternions are negated
  when their dot with vertex 0's is negative.
- Per light: L = directional ? P : P + view (view unnormalised); len = |L|; normalise L; V = normalise(view);
  H = V + L. Inputs NH = n.norm(H), VH = V.norm(H), NV = n.V, LN = L.n, SP = L.(spot/2047), CP (Config7) =
  (norm(H) - n(n.norm(H))).t. ndl = two-sided ? |LN| : max(LN,0). spec0 = D0*specular0; RGB reflection from
  RR/RG/RB (missing ones fall back to RR or 1); spec1 = D1*refl*specular1; geometric factor min(ndl/|H|^2,1) on
  spec0/spec1 when the light's bits are set; clamp-highlights zeroes specular when ndl == 0.
  diffuse += (diffuse*ndl*shadow_p + ambient)*dist*spot; specular += (spec0+spec1)*clamp*dist*spot*shadow_s.
  Fresnel (FR LUT) at the last light sets diffuse and/or specular alpha. Then diffuse.rgb += global ambient.
  Sums start at (0,0,0,1). Output to 8 bits by clamp(0,1)*255, truncated: primary = diffuse, secondary = specular.

## Fog (Azahar software renderer)
- 0x0E0 bits 0-2 mode (5 fog, 7 gas), bit 16 flip; 0x0E1 fog colour (r bits 0-7, g 8-15, b 16-23);
  0x0E6 LUT index (advances by 1 per data write, used modulo 128); 0x0E8-0x0EF LUT data.
- Entry: value bits 13-23 (/2047), signed difference bits 0-12 (/2047).
- After the alpha test: index = clamped fragment depth * 128 (or (1 - depth) * 128 if flipped); i = clamp(floor, 0, 127);
  factor = clamp(value + difference * (index - i), 0, 1); rgb = (u8)(factor * rgb + (1 - factor) * fog colour); alpha unchanged.
- Gas mode is not implemented.

## Cube maps (Azahar software renderer)
- Unit 0 type 1 (cube) or 4 (shadow cube). +X face at the texture address (0x085); the other faces at 0x086-0x08A
  (-X, +Y, -Y, +Z, -Z): low 22 bits from the face register, bits 22-27 from 0x085; times 8.
- Direction (u, v, w) = (tc0.u, tc0.v, tc0.w): the largest |component| picks the face; +X: (x, y, z) = (-w, -v, u);
  -X: (-w, v, u); +Y: (u, w, v); -Y: (-u, w, v); +Z: (u, -v, w); -Z: (u, v, w); face coords = (x/z, y/z) * 0.5 + 0.5.
## Texture LOD (0x084 etc.): bias bits 0-12 signed 1.4.8, max level bits 16-19, min level bits 24-27.
  Azahar's Vulkan renderer ignores the bias; this port applies it in the Vulkan "console" filter mode.
