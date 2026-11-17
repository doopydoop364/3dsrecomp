# Phase 6 report (2026-11-16 night to 2026-11-17)

What changed since phase 5, what each game does now, and what is open. Every change is generic (no per-game code);
per-game files hold only test scripts.

## Games

| Game | Code | Status |
|---|---|---|
| Kirby: Planet Robobot | ARM | unchanged: regress 12/12, smoke pixel-identical after every change |
| Mario Kart 7 | ARM | unchanged (smoke identical); its 44 geometry-shader draws per race now render |
| Ocarina of Time 3D | ARM | unchanged (smoke identical) |
| Xenoblade Chronicles 3D | ARM | unchanged (smoke identical) |
| Tomodachi Life | ARM + Thumb | **new**: boots, first-run screens, touch input, save created, date/time screens |
| Monster Hunter 4 Ultimate | ARM + Thumb | **new**: save and extra data created, Circle Pad Pro check, Capcom logo, title menu, character creation (3D model, keyboard) |
| Super Smash Bros. | ARM + Thumb + 1 CRO | **new**: runs until it loads its CRO module (phase 7) |

## Recompiler: Thumb
- 16-bit Thumb (ARMv6K: Thumb-1 plus sxt/uxt, rev, cps) is translated beside ARM.
- A function is named by its *key*: its address, with bit 0 set for Thumb. Calls, tail branches and code pointers
  carry the instruction set, so ARM `blx imm`, Thumb `bl`/`blx` pairs, `bx`/`blx rX` and `pop {pc}` interwork
  through the same lookup (`recomp_lookup` picks the Thumb table on bit 0).
- Discovery: Thumb pointers found in data must look like a function start (a push of lr, or a short body that
  returns), because nearly any halfword is valid Thumb. Addresses that are certainly code are taken without that
  check: veneers `ldr/adr rX ; bx rX`, ARM entry stubs `add r12, pc, #1 ; bx r12`, position-independent
  `ldr rX, =off ; add rX, pc`. The SDK's static-initialiser walker is recognised in its Thumb form too.
- Checked against a reference CPU: `tests/difftest.py --title <id> --thumb` runs every Thumb function of Tomodachi
  Life in Unicorn and natively on random inputs: all 342 completed runs match (100%). A 3,000-function ARM sample
  matches too (its one mismatch was a function discovery had missed, since fixed).

## Runtime
- **Geometry shaders** (point mode, as Azahar): a second shader unit with its own uniforms, program and swizzles;
  SETEMIT/EMIT produce triangles. MH4U runs its interface through them. Variable-primitive and subdivision modes
  still log and skip (no game has needed them yet).
- **Procedural textures** (texture unit 3), in both renderers, as Azahar's reference. MH4U draws its text with them.
- Kernel: ControlMemory MAP/UNMAP (memory aliases).
- fs: archives are closed and their handles reused (MH4U ran out); GetFreeBytes; another title's save data is that
  game's save if it is in `work/` (so a demo's save could carry over), else "not found".
- The shared system font sits after the application's memory and is relocated there (as Azahar); a New 3DS title
  or an old one with a big linear heap (Smash) no longer collides with it. Robobot's text renders the same.
- The 3DS clock keeps local time (it showed UTC).
- Vulkan: a draw that sampled a texture overlapping a dirty render target submitted the command buffer midway and
  crashed the NVIDIA driver; fixed.
- `system/sysarchives/000400db00010302.romfs`, the NG word list a console always has, is generated as an open
  replacement when you have not dumped yours (byte-identical to the one Citra and Azahar ship). Tomodachi Life and
  MH4U need it.

- **Busy waits no longer stop time.** A thread that runs about 64 ms of 3DS CPU without a system call is treated
  as busy-waiting: further work is charged to the virtual clock (not to the wall-clock pacing, so loads stay fast)
  and threads on other cores get turns. MH4U waited forever after its logo without this. Long loads in MK7 and
  Xenoblade now take virtual time as on a console (their later frames shift slightly); Robobot regress 12/12.
- `RB_ALLOW_ATTACH=1` lets gdb/perf attach to a test run without changing the system's ptrace setting.
- Gamepad face buttons: Nintendo (by position) or Xbox (by label) layout, in the Esc menu or `controls.gamepad_layout`.

## Open
- Tomodachi Life says it found demo-version data on a fresh console. The two "other title" save checks now say not
  found; the remaining trigger is not understood yet. Answering No continues normally.
- APT Wrap/Unwrap (data encryption) is a stub; Tomodachi Life calls it 520 times at boot.
- MH4U's geometry-shader path runs single-threaded (a vertex shader run per index, no sharing); worth a
  performance pass before playing it.
- Smash needs phase 7 (CRO modules).
- `ir:rst` (New 3DS ZL/ZR/C-stick) is still unverified: no game has opened it yet.

## Next (roadmap order)
Phase 7 (CRO modules: Pokémon X, Animal Crossing, Smash), then updates and DLC, then the rest of the GPU features.
