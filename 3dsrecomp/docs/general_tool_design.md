# 3dsrecomp: from "a Robobot port" to "a tool that ports my CIAs"

Goal: one command that takes a CIA the user dumped and produces a native Linux executable for it.

    3dsrecomp build "Some Game.cia"      ->  out/<title-id>/game  (+ game data folder)
    3dsrecomp run   "Some Game.cia"      ->  build if needed, then start it

Realistic promise: works unattended for many games, and needs a small per-game file for the
stubborn ones. Not "any CIA, untouched". Output is for the user's own dumps only: share the tool,
never what it produces.

## 1. Pipeline

| Stage | Input | Output | State today |
|---|---|---|---|
| 1 extract | CIA / CCI / CXI (+ seeddb.bin) | code image, ExHeader, RomFS | done: `3dsrecomp extract` (phase 2) |
| 2 analyze | code image, ExHeader | function entry list, special-case list | REA/Ghidra list + pointer scans, partly manual |
| 3 recompile | code image, lists | C files | works, Robobot layout hard-coded |
| 4 compile + link | C files, shared runtime | executable | works (Makefile) |
| 5 run | executable, game data | the game | Phase 3 to 5 in progress |

Every stage reads and writes a per-game work folder: `work/<title-id>/`. Stages are cached by the
SHA-256 of the code image plus the tool version, so a second build does nothing.

## 2. What must stop being hard-coded

Currently fixed for Robobot, to be read from the ExHeader or the per-game file instead:

- `recomp/armrecomp.py`: `BASE`, `TEXT_END`, `RO_END`, and `NONLOCAL_RETURN = {0x232cf4: ...}`
- `runtime/recomp.h`: `RECOMP_TEXT_BASE`, `RECOMP_TEXT_SIZE`
- `runtime/host.h`: `LINEAR_BASE` (depends on kernel version in the ExHeader), stack size, main thread priority
- `analyze/find_pointer_functions.py`: static-initialiser table range `0x675158..0x6784e0`
- `runtime/services.c`: title id in the APT reply, save folder name
- `tests/`: region addresses

Rule from now on: no address or size of a specific game in `.c`, `.h` or `.py` files. They live in
generated data.

## 3. Per-game file

`games/<title-id>.toml`. Stage 2 writes it; a person may add to it. Hand-written entries are kept
when the stage runs again.

    [title]
    id = "0004000000183600"
    product = "CTR-P-AT3A"
    code_sha256 = "c53ab005..."

    [layout]            # copied from the ExHeader, never typed by hand
    text = [0x100000, 0x5784e0]
    rodata = [0x679000, 0x66530]
    data = [0x6e0000, 0x2aaf0]
    bss = 0x12d8c
    stack = 0x10000
    kernel_version = "2.50"

    [functions]
    extra = [0x1e51cc]                  # entries no scan found (added by the runtime report, see 5)
    not_code = [0x2eb3b4, 0x48db68]     # data that scans mistake for code

    [[special]]                         # idioms the detectors recognised
    kind = "inline-data-call"           # callee reads words after the bl and resumes the caller
    target = 0x232cf4
    resume_offsets = [8, 12, 16, 20]

    [[special]]
    kind = "relative-init-array"
    range = [0x675158, 0x6784e0]

    [enhancements]                      # optional, per game
    widescreen = false

## 4. Analysis without a person in the loop

Function discovery, in order, each feeding the next:

1. entry point and direct call targets (recursive descent)
2. words in data and in uncovered `.text` that point at cleanly decodable code
3. `ldr =addr`, `adr`, and `ldr`+`add pc` address constants inside discovered code
4. detectors for known idioms (below)
5. optional: Ghidra headless or REA list, merged in when available. The tool must work without it.

Idiom detectors replace the hand-found special cases. Each matches instruction patterns, not
addresses:

- relative init array: the runner loop `ldr r0,[r4]; add lr,pc,#4; add r0,r0,r4; mov pc,r0`
- fplib inline-data dispatcher: function that starts with `ldr r4,[lr],#4`
- hand-made return address: `add/sub lr, pc, #imm` (already generic)
- jump tables: `add pc,pc,rN,lsl #2` and `ldr pc,[pc,rN,lsl #2]` with a bounded index (already generic)

A detector that fires records a `[[special]]` entry, so the result is visible and editable.

## 5. Missing code found at run time

Static analysis cannot find everything. The runtime handles a jump to an unknown address like this:

1. Log the address to `work/<title-id>/missing.txt` and stop with a clear message (today's behaviour).
2. `3dsrecomp run` sees the file, adds the entries to `[functions].extra`, rebuilds only the affected
   C file, and starts the game again. First runs of a new game stumble a few times, then settle.
3. Later, if restarts are too annoying: compile the one function to a small shared object and load it
   into the running process, so the game does not restart.

No interpreter and no JIT: the fallback is still ahead-of-time compiled code.

## 6. Features other games need that Robobot does not

| Feature | Why | Size |
|---|---|---|
| Thumb instruction set | Many games mix ARM and Thumb. Robobot has none. | large, one time |
| CRO modules (`ldr:ro`) | Code loaded at run time (Pokemon, Smash). Recompile each module, link by its import/export tables. | large |
| More services | camera, microphone, gyroscope, networking, StreetPass, Miis | medium each, most can answer "not available" |
| Shared system font | Games that draw text with the system font need a substitute font file | small |
| New 3DS extras | extra buttons, more memory, faster CPU flag | small |
| Renderer coverage | each game exercises different PICA200 features | ongoing |

## 7. Repository shape

    3dsrecomp/
      cli.py                 the one command (build, run, clean, info)
      extract/               CIA, NCCH, seed crypto, ExeFS, RomFS
      analyze/               discovery passes, idiom detectors, writes games/<id>.toml
      recomp/                ARM (later Thumb) to C
      runtime/               kernel, services, renderer, audio, input: no game-specific code
      tests/                 differential tests, driven by the per-game file
      games/                 per-game files (safe to share: addresses only, no code)
      work/                  generated, never committed

## 8. Order of work

1. Finish Robobot (Phases 3 to 5) so the renderer and audio are proven. Follow the rule in section 2
   for all new code.
2. Move the existing hard-coded values into `games/0004000000183600.toml` and the ExHeader reader.
3. Wrap the stages in `3dsrecomp build/run`, with the missing-function loop from section 5.
4. Second game on the same engine (for example another HAL title) to shake out assumptions.
5. Thumb, then CRO, then whatever the third game needs.

## 9. Not goals

- Running without the user's own dump, or shipping translated game code or assets.
- Windows or macOS builds (Linux only for now).
- Cycle-accurate timing. The runtime aims for "the game behaves the same", not hardware accuracy.

## 10. Plan (2026-11-16, after Robobot became playable)

State: Robobot plays through at 60 fps (Vulkan renderer, audio, video, amiibo, menu). Section 2's hard-coded values are
gone: the layout comes from the ExHeader, special cases from idiom detectors. What still needs a person: decrypting
and extracting the CIA (separate scripts, seed by hand) and the REA-made function list (`games/<title id>/functions_rea.txt`).

Each phase ends with a check that runs without a person. Robobot stays the regression game throughout: every phase
must still build it and pass `tests/regress.sh` plus the scripted stage-1 run.

| # | Phase | Delivers | Done when |
|---|---|---|---|
| 0 | Housekeeping (done) | root `.gitignore` for dumps (done), commits under the GitHub no-reply address, push the branch | the work is backed up on GitHub |
| 1 | Repository shape (done) | `robobot-port/` becomes the generic layout of section 7 (`3dsrecomp/` with extract, analyze, recomp, runtime, tests, games); Robobot is just `games/0004000000183600.toml` | Robobot builds from the new tree, regress passes |
| 2 | Extract stage (done; .3ds untested for lack of a sample) | `3dsrecomp extract game.cia`: decrypt (reusing `decrypt_cia.py`), seeds from `seeddb.bin`, ExeFS, code decompression, RomFS, icon/title, into `work/<title-id>/` | Robobot's CIA extracts to byte-identical files |
| 3 | Analysis without REA (done: Robobot's stage-1 run is identical; Mario Kart 7 and Tomodachi Life discover 32k and 42k functions) | recursive descent, data and constant pointer scans, idiom detectors, jump tables; writes `games/<id>.toml` | Robobot builds and plays with `functions_rea.txt` deleted; a coverage report shows what each pass found |
| 4 | `3dsrecomp build/run` (done; the missing-function restart is not yet exercised by a real game) | one command, cached stages, the missing-function loop of section 5 (log, add, rebuild one file, restart) | `3dsrecomp run Robobot.cia` from a clean checkout reaches gameplay |
| 5 | Second game | an ARM-only title from the user's library; fix the services, PICA features and idioms it needs | it reaches its first gameplay; audit counters show which PICA features it uses |
| 6 | Thumb | Thumb decoding and translation in the recompiler, interworking (`bx`, `blx`), difftest coverage | a Thumb-using game builds; difftest passes on its Thumb functions |
| 7 | CRO modules | recompile each `.cro` from RomFS at build time; `ldr:ro` service links them by their import/export tables | a CRO game (e.g. Pokemon, Smash) loads its modules |
| 8 | Polish | game library / launcher, per-game defaults vs global settings, shared system font, packaging | a new user can go from a dump to playing with one command |

Not in this plan: missing PICA features are added as games need them (the audit counters say which), the same for
services. Save states and widescreen stay on the PC-experience list.

### Findings so far (2026-11-16)
- Discovery from the entry point finds 60,141 functions in Robobot (the REA list had 54,922; all but 24 of its
  functions are found or covered inside others) in about 20 seconds.
- Mario Kart 7 (0004000000030800): ARM only, no CRO, 32,024 functions, 16 untranslated instructions: the natural
  second game for phase 5.
- Tomodachi Life (000400000008c300): about 3,000 ARM-to-Thumb calls, so it needs phase 6 first; no CRO.
- First `3dsrecomp run` of Mario Kart 7: builds and starts, stops at svc 0x38 GetResourceLimit (phase 5 starts there).

### Phase 5 results (2026-11-16)
All three games run with generic changes only (no per-game code or hints):
- **Mario Kart 7**: title, menus, kart builder, races (3D track, HUD, map, rivals). Needed: resource-limit and
  affinity SVCs; the linear heap at 0x14000000 for kernels older than 2.44 (ExHeader kernel version, as Azahar);
  save data "not formatted" until FormatSaveData; offline cecd/boss/frd/ac; jump tables whose guarding `cmp` is far
  back or has VFP code in between. Open: 44 geometry-shader draws per race are skipped (PICA geometry shaders are
  not implemented); a "no Mii characters" notice on boot (empty Mii database, as on a console without Miis).
- **Ocarina of Time 3D**: title, file select, name entry, intro and Kokiri Forest cutscenes. Needed:
  FormatThisUserSaveData.
- **Xenoblade Chronicles 3D**: title, menu, opening cutscene. Needed: New 3DS memory modes from the ExHeader
  (124 MB; `system.new_3ds`, default on, as Azahar), CheckNew3DS, ir:USER without an accessory, ir:rst (ZL/ZR,
  C-stick; bound to keys and the gamepad triggers / right stick, not yet seen used by a game), exclusive memory-fill
  end. Robobot also asks for New 3DS mode and now gets it; its regress and smoke stay identical.
- StreetPass: a title's box info reads as an existing empty box when none was saved, so games do not ask to create
  StreetPass data on every boot (nothing writes it back yet).

### Test library (user's dumps in ~/3ds-games, 2026-11-16)
| Game | Title id | Code | ARM->Thumb calls | CRO/CRS files | Needs |
|---|---|---|---|---|---|
| Kirby: Planet Robobot | 0004000000183600 | 5.7 MB | 2 | 0 | plays |
| Mario Kart 7 | 0004000000030800 | 5.2 MB | 3 | 0 | phase 5 |
| Ocarina of Time 3D | 0004000000033500 | 4.0 MB | 3 | 0 | phase 5 |
| Xenoblade Chronicles 3D (New 3DS, key slot 0x18) | 000400000f700100 | 3.6 MB | 7 | 0 | phase 5 + New 3DS mode |
| Tomodachi Life | 000400000008c300 | 7.7 MB | 3052 | 0 | phase 6 |
| Monster Hunter 4 Ultimate | 0004000000126300 | 13.5 MB | 1919 | 0 | phase 6 |
| Super Smash Bros. for 3DS (+ DLC 0004008c000edf00) | 00040000000edf00 | 10.0 MB | 6864 | 1 | phase 6 (7) |
| Animal Crossing: New Leaf - Welcome amiibo | 0004000000198e00 | 7.6 MB | 5445 | 39 | phases 6, 7 |
| Pokemon X | 0004000000055d00 | 4.4 MB | 1373 | 134 | phases 6, 7 |
All nine extract with `3dsrecomp extract` (key slots 0x2C, 0x25, 0x18; Robobot with a seed).

## 11. Phase 7: CRO modules (design, 2026-11-17)

**What a CRO is.** A relocatable module (like a shared library) the game reads from its RomFS (or builds in memory)
and hands to `ldr:ro`, which maps it at an address the game chooses, rebases its tables, relocates it and links it
with the main program (described by `static.crs`) and the other loaded modules; the game then calls its `OnLoad`.
Format and rules follow Azahar's `cro_helper.cpp` (header of 0x138 bytes after a 0x80-byte hash area; segments
addressed by segment tags; named / indexed / anonymous exports and imports; relocation batches).

**Facts from the library** (parsed from the dumps): every relocation in Smash, Pokémon X (134 modules) and Animal
Crossing (39) is type 2, an absolute address written into a data word. Relocations never patch instructions.
Smash's one module is not a plain `.cro` file in its RomFS, so modules are captured from memory when loaded.

**Design.**
- `ldr:ro` in the runtime: Initialize (map and rebase `static.crs`), LoadCRO / LoadCRO_New (map, rebase, internal /
  static / exit relocations, link imports both ways, register in the module list, fix), UnloadCRO, LinkCRO /
  UnlinkCRO, Shutdown — the same semantics as Azahar.
- Module code is recompiled **position-independent**: every address the generated code computes (pc reads, return
  addresses, literal addresses, table cases) is `A(offset)`, which is `offset + mb` with `mb` the module's current
  base. One build per module file, whatever address the game loads it at, any number of times. The main program
  uses the same emitter with `A(x) = x`, so its generated code is unchanged.
- Literal words are not folded into constants in module code (relocations write them). In the main program, the
  words `static.crs` relocates are excluded from folding too.
- Discovery in a module starts from its exports, `OnLoad` / `OnExit` / `OnUnresolved`, and every internal relocation
  whose symbol is in the code segment (vtables, callbacks): an exact list of code pointers, then the usual closure.
- Function lookup: an address outside the main program is looked up in the loaded modules (by range) and their
  offset tables. A module that loads without compiled code is saved to `work/<id>/cro/<name>.cro` and reported;
  `3dsrecomp run` rebuilds with it and restarts, like the missing-function loop.

## 12. Updates and DLC (2026-11-19)

**How a console runs them** (Azahar's `loader/ncch.cpp`, `archive_selfncch.cpp`, `archive_ncch.cpp`, read for this):
an installed update (title `0004000E` + the game's low word) supplies the ExHeader and `.code` that run; the game's
RomFS stays the base one (SelfNCCH type 0) and the game opens the update's RomFS itself as SelfNCCH type 5 (the base
RomFS when there is no update) and layers the two. DLC (`0004008C` + low word) is read through the content archive
`0x2345678A`: archive path {title id, media type}, file path {open type, TMD content index, section, ExeFS name};
a content that is not installed is "not found". Games may also ask `am:app` about DLC; none of the library does.

**What 3dsrecomp does.**
- `3dsrecomp build|run` take several dumps (`run game.cia update.cia dlc.cia`) and also pick up an update or DLC dump
  lying in the game dump's folder, recognised by title id. Each is added to `work/<id>/game/` once (again only for a
  newer title version; `title.json` records what was added).
- Update: its ExHeader, `code.bin` and `code_decompressed.bin` replace the game's (the base ones are kept in
  `game/base/`), its RomFS becomes `game/update_romfs.bin`. Discovery, recompilation and the stage cache follow the
  program files, so the update's code is what gets recompiled; CRO modules come from the update's RomFS first.
- DLC: every content's RomFS, decrypted, as `game/dlc/<TMD content index>.romfs` (indices, not content ids: they
  differ when ids are skipped).
- Runtime: SelfNCCH type 5 serves `update_romfs.bin` when present; archive `0x2345678A` serves the game's DLC
  contents and the system archives, all from their IVFC level-3 data (the exact level-3 size from the IVFC header).

**Findings.** Smash's DLC dump (139 contents) holds no fighter data: every content but the index (content 0: icons,
content info) is a 14-byte "content data" marker of ownership. The fighters come with the game's update, and the
library's Smash (title version 2080) has no update dump, so its code never asks for DLC. Checked without it: the
update path end to end with a stand-in (the base dump extracted as an update; Smash running with the base RomFS
served as the update RomFS); the DLC unpacking (all 139 contents decrypt to IVFC).
