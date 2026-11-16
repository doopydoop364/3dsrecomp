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
| 1 extract | CIA (+ seeddb.bin) | code image, ExHeader, RomFS | works (`tools/extract_seeded.py`), seed passed by hand |
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
- `tools/find_pointer_functions.py`: static-initialiser table range `0x675158..0x6784e0`
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
