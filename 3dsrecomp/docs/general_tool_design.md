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
| 4 | `3dsrecomp build/run` | one command, cached stages, the missing-function loop of section 5 (log, add, rebuild one file, restart) | `3dsrecomp run Robobot.cia` from a clean checkout reaches gameplay |
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
