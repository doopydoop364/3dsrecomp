# Robobot native port (work in progress)

Native Linux port of Kirby: Planet Robobot (CTR-P-AT3A) by static recompilation: the game's ARM11
code is translated to C ahead of time and compiled for x86-64. No emulated CPU. Game data is read
from your own dump in `game/` and is never committed (this folder is git-excluded).

## Layout
- `tools/` extraction: `extract_seeded.py` (CIA -> `game/`, needs the title seed), `romfs_list.py`, `svc_scan.py`,
  `find_pointer_functions.py` (finds code only reachable through stored pointers)
- `recomp/armrecomp.py` the recompiler (ARMv6K + VFPv2, ARM mode -> C)
- `recomp/layout.py` reads the memory layout and title id from `game/exheader.bin`; also writes `build/gen/layout.h`
- `recomp/idioms.py` pattern detectors (inline-data dispatcher, relative static-initialiser array); results go to `build/gen/specials.json`
- `games/<title id>.toml` hand-written per-game settings (`[functions] extra` and `not_code`)
- `recomp/functions_rea.txt` function entries from REA/Ghidra; `functions_extra.txt` entries found by pointer scan
- `runtime/recomp.h`, `runtime/vfp.c` CPU state, memory access, flag/shift/VFP helpers
- `tests/difftest.py`, `tests/runner.c` differential test against a reference ARM11 emulator (Unicorn, test-only)
- `docs/phase1_recon.md` recon report; `build/` generated C, objects, test binary

## Rebuild
    python3 recomp/armrecomp.py game/code_decompressed.bin recomp/functions_rea.txt build/gen
    make -j12
    .venv/bin/python tests/difftest.py --variants 6 --seed 1

## Controls and settings
Copy `settings.example.toml` to `settings.toml` in the folder you run from (or point `RB_SETTINGS` at it).
Defaults: WASD circle pad, arrow keys d-pad, I/J/K/L = X/Y/B/A (a diamond like the 3DS), Q/E = L/R,
= Start, - Select; gamepads by Nintendo button position; left mouse on the bottom screen is the stylus.
Hotkeys: F1 next layout (stacked, large top, top only, side by side), F2 swap screens, F3 show the bottom
screen in top-only mode, F11 fullscreen, F12 screenshot, Escape quit. `RB_INPUT` scripts input for tests.

## Audio
`runtime/dsp.c` decodes the game's 24 DSP voices (PCM8, PCM16, DSP-ADPCM), resamples them linearly, mixes the three
intermediate mixes down to stereo and reports voice status back to the game; `runtime/audio.c` plays it through SDL at
32728 Hz. settings.toml: `[audio] enabled = true`, `volume = 100`. `RB_WAV=out.wav` records the output (also headless).
Not done yet: per-voice filters, gain ramps, delay/reverb aux effects, polyphase interpolation.

## Rule for new code
No address or size of a specific game in `.c`, `.h` or `.py` files. Layout comes from the ExHeader
(`layout.h` for C code), idioms from detectors, and anything a person decides goes in `games/<id>.toml`.
Hardware addresses that are the same on every 3DS title (VRAM, config page, TLS, DSP memory) stay in the runtime.
Code the analysis missed is appended to `game/missing_functions.txt` when the game hits it at run time.

## Run
    ./build.sh                       # regenerate C from the game image and rebuild
    ./build/robobot game             # boot the game (RB_LOG=2 or 3 for more detail)
    python3 tools/disasm.py 100000   # disassemble guest code

## Status
Phase 2 (recompiler): 54,199 functions / 1,295,727 instructions translated, 41 unsupported instructions
(data mistaken for code). Whole game compiles in about 25 s. Differential tests against Unicorn match on
all real code; the two known mismatches (002eb3b4, 0048db68) are string data listed as functions.
About 40% of functions cannot be checked this way (the reference run does not complete on random inputs).

Phase 3 (OS layer, in progress): `runtime/kernel.c` has the address space, cooperative guest threads,
mutex/semaphore/event/timer/address-arbiter objects and the supervisor calls. `runtime/services.c` has
srv:, APT, cfg, ptm, fs:USER (RomFS + host-folder saves), hid:USER (no input yet), gsp::Gpu (commands are
acknowledged, nothing is drawn) and dsp::DSP (handshake and frame interrupt only, no mixing, source
status is never updated).

Boot: the game now reaches its main loop. It runs the static constructors and SDK start-up, opens
RomFS, completes the DSP handshake, initialises the Mii library, loads the system font, creates its
save data and extra data, and then submits GPU command lists continuously (about 19 per frame).
There is no input and audio is silent.

Phase 4 (renderer, in progress): software renderer in `runtime/pica.c` (command lists, vertex loading,
clipping, rasteriser, textures incl. ETC1/ETC1A4, texture combiners, alpha/stencil/depth tests, blending,
logic ops) and `runtime/pica_shader.c` (vertex shader interpreter). Facts used are in `docs/pica_facts.md`.
Title screen: renders like the reference (Azahar and the user's image): Robobot, planet, light streaks,
logo, UI. The cause of the earlier differences was a 64-jump cap in the command-list processor; the
game's 3D scene is a chain of ~174 jumping lists per pass. Remaining visible difference: fragment
lighting is a placeholder (flat white), so lit surfaces lack shading, e.g. the planet's green land and
bright rim.

GPU replay test (`tests/gpu_replay.c`, `make build/gpu_replay`): replays one command list captured from
Azahar on its own memory and compares the render target pixel for pixel. Capture with
`tools/azahar_capture.gdb` (Azahar started with `-g 24689`; CAPTURE_LIST / CAPTURE_MIN_SIZE / CAPTURE_COUNT),
take the matching PICA state from the port with `RB_GX_LOG=<frame>` (build/replay/state_NN.bin), then
`build/gpu_replay <state> <vram> <linear> <list> <size> <out> <reference vram>`. Captures in build/ref/capture*
match exactly for the 2D lists and the top 3D pass (2 px), bottom 3D pass differs only by lighting.
Helpers: `tools/dump_framebuffer.py`, `tools/dump_texture.py`, `tools/make_cxi.py`.

Debug switches: `RB_FLAT=1` one flat colour per draw call; `RB_PICA_LOG=n` and `RB_PICA_LOG_FROM=k` log draws
k+1..k+n with clip extents, rejected fragments and bone matrices; `RB_PICA_DUMP=n` writes the shader and
uniforms of draw n to build/shader_dump.bin for `tools/shader_trace.py`; `RB_SHOT=frame,...`; `RB_HEADLESS=1`.
Speed: about 15 frames per second; the game runs at a quarter of its normal speed.
Not done: fragment lighting (placeholder: white), fog, linear texture filtering, cube/shadow textures,
procedural textures, geometry shaders, input, audio.

Hand-written code (the video decoder around 0x363000-0x372000) uses jump tables of self-relative offsets,
hand-made return addresses built in one or two steps, and returns into the middle of other functions.
The recompiler compiles `add pc, rX, rTable` tables as switches; `tools/find_pointer_functions.py` adds
table targets, two-step addresses and missed prologues as entries; a call that comes back to an
unexpected known address continues there (`recomp_continue`). GE flags and SEL are implemented.

System data the game needs besides its own dump (all under `game/sysarchives/`):
- `0004009b00010202.romfs`: shared Mii data, from the user's console. In place.
- `shared_font.bin`: the shared system font image. Currently a STAND-IN made from the game's own font:
      python3 tools/make_shared_font.py game/romfs.bin game/sysarchives/shared_font.bin font/ALL/CommonStd.bcfnt.cmp
  For the real one, run the same tool on the font system archive from the console (title 0004009b00014002).
  Without any font file the runtime stops with a clear error; an empty font crashes the game.
- `0004009b00010602.romfs`: requested once, not supplied; the game carries on without it so far.

## Plan
Phases: (1) recon, (2) recompiler, (3) OS layer, (4) renderer, (5) input, audio, saves, (6) polish.
PC features follow `docs/pc_experience_design.md` and its schedule:

| When | Build |
|---|---|
| Now (Phase 4) | finish the title screen against the reference emulator (Azahar, test-only) |
| With Phase 5 input | settings file, window and fullscreen, gamepad, keyboard, mouse touch, layout compositor (stacked, large top, top only) |
| Vulkan renderer design | surface-cache architecture from the start (see the design doc's last section) |
| After Vulkan works | resolution scale, filtering, anti-aliasing |
| After Robobot plays end to end | overlay menu, touch hotspots, hooks (skip intro, hide prompts, glyphs), save states |
| With the 3dsrecomp general tool | universal defaults vs per-game profiles, game library |
| Last | per-game widescreen, beyond 60 fps |

Rules: every enhancement can be switched off and "original" mode must match the reference
emulator; enhancements never change game timing (virtual clock); per-game data before per-game code.

## Known gaps
- Stubbed and answering "success" during boot: act:u (accounts), boss:U (SpotPass), nfc:u, cecd:u (StreetPass),
  cfg:u commands 0x8 and 0xa, APT command 0x58, fs DeleteExtSaveData. Config blocks 00170000 and 000c0000 return zeros.
- Shared extra data f000000b is created empty; the game creates CFL_DB.dat and gamecoin.dat in it itself.
- The function lists `recomp/functions_rea.txt` and `recomp/functions_extra.txt` are Robobot data stored in the
  tool folder; for the general tool they belong in per-game storage.
- `game/missing_functions.txt` only records misses from indirect calls and unknown direct branches. A computed
  jump (`IJUMP`) to an unknown address inside .text is treated as a return to an outer frame and not recorded,
  because recording every such target would add false entries.
- Unknown service commands answer "success" and are logged as STUB; unknown config blocks return zeros.
- hid:USER reports no buttons, so the game stays on the title screen. Directory listing returns no entries.
- ControlMemory only commits memory (no free/map/mirror). Threads are never preempted.
- STREX always succeeds (cooperative threads). GE flags, FPSCR cumulative exception bits and VFP
  short-vector mode are not modelled (vector mode traps).
- About 18% of .text is not covered by any function: literal pools, inline data, and possibly code reached
  only through pointers computed at run time. A missing entry stops the run and prints the address.
- Calls to the fplib NaN/Inf dispatcher (0x232cf4), hand-made return addresses and the static-initialiser
  table (0x675158) are special-cased.

## Scripted path to gameplay
`tests/play.sh "<RB_INPUT>" "<RB_SHOT frames>" <exit frame>` restores `build/pristine/` (a save the game made on a clean
boot: copy `game/save`, `game/extdata`, `game/extdata_shared` there) before every run, because the game writes its save
and a changed save changes the menu flow. `tests/sheet.py` (venv python) makes a contact sheet of the screenshots.
This input reaches stage 1 of Patched Plains and walks right with a jump (about 1.5 minutes with RB_FAST):

    1250-1255:A;1330-1335:A;1410-1415:A;1490-1495:A;1570-1575:A;1650-1655:A;1700-1705:START;1760-1765:A;
    1850-1855:A;2300-2305:A;2800-2870:RIGHT;2950-2955:A;3200-3500:circle=154/0;3300-3320:B

Title, file select, data-sharing prompt (No), Story Mode, START skips the intro movie, A on the world map enters Area 1,
the d-pad walks to door 1, A enters, then the circle pad walks and B jumps.
The intro movie plays: decoded frames go through the YUV-to-RGB converter service (`runtime/y2r.c`).
Open question: a save written by an earlier run made the area entry bounce back to the world map (Kirby's star flew
off again). That save is kept in `build/savesnap_after/`; it may point to a save-write bug or just a different saved state.
`RB_LOG_READS=1` prints every RomFS read with its frame; `RB_LOG_FROM=<frame>` raises the log level to 3 from that frame.
