# Roadmap to 3dsrecomp 1.0

Agreed with the user on 2026-11-16; order of the remaining work agreed on 2026-11-19 (below "Done"). Status is kept
current as work lands; the phase details of the general tool are in `general_tool_design.md` section 10.

**Scope of 1.0 (the user's rule, 2026-11-19):** the GPU, the audio DSP and the system services are complete, not
just what the 8 library games happen to use, so games added later work too. A feature no library game uses is
checked against Azahar and the reference software renderer with a test case of its own (a synthetic PICA command
list, DSP input or service call), the way `tests/regress.sh` replays captured GPU work.

## Done
- Robobot fully playable (Vulkan renderer, audio, amiibo, PC settings, profiler).
- Phases 0-5 of the general tool: `3dsrecomp build|run <dump>`, extraction with seeds, discovery from the entry
  point, cached build stages; Mario Kart 7, Ocarina of Time 3D and Xenoblade Chronicles 3D boot and play.

## Done since (2026-11-19)
- Overlap-aware surface cache: render targets that share VRAM coexist and hand over tiles on the GPU.
  tests/bench.sh: MH4U 10.4 -> 6.2 ms/frame, Xenoblade 8.6 -> 4.2. Design: `pc_experience_design.md`.
- Live control (`tools/play.py`, `RB_CONTROL`): a running game takes commands (run, press, hold, touch, shot)
  and waits, paused, for the next, so scripts and agents can play it step by step.

## 1. Phase 6: Thumb code — *done (2026-11-17)*
16-bit Thumb (ARMv6K has no Thumb-2) beside ARM in the recompiler and in discovery; interworking through function
keys (bit 0 = Thumb). Every completed Thumb run of Tomodachi Life matches Unicorn (difftest --thumb).
Tomodachi Life boots to its first-run screens (touch works); Monster Hunter 4 Ultimate runs (needed geometry
shaders); Smash runs until it loads its CRO module (phase 7). Closed 2026-11-19: APT Wrap/Unwrap are implemented
(AES-CCM); Tomodachi Life's "demo-version data has been found" on a fresh console is what Azahar shows too.

## 2. Phase 7: CRO modules — *done (2026-11-18)*
`ldr:ro` in the runtime (Azahar's rules, our code); module code recompiled once, position-independent (every address
is the module's current base plus an offset), so modules run wherever and however often the game loads them.
Smash reaches its character select; Pokémon X plays its intro with modules loading, unloading and reloading.
Design and findings: `general_tool_design.md` section 11.

## 3. Updates and DLC — *done (2026-11-19), tested end to end with Smash's update and DLC*
`3dsrecomp build|run` take the game plus update (0004000E) and DLC (0004008C) dumps, given together or lying in the
same folder. The update's ExHeader and code replace the program before discovery (base kept in `game/base/`); its
RomFS is served as SelfNCCH type 5 (the game layers it over the base RomFS itself, as on a console); DLC contents are
served through the content archive 0x2345678A by content index, and `am:app` answers which DLC contents exist and are
owned from the dump's TMD and ticket (`game/dlc/info.bin`, as Azahar decides). Smash with update 35296 and its DLC:
the DLC fighters, Mii outfits and music unlock and appear at character select. Design: `general_tool_design.md`
section 12.

## Stage A: playtesting
1. Saves outside the game folder — *done (2026-11-19)*: `~/.local/share/3dsrecomp/<title id>/` (save, extra data,
   SD card, StreetPass, amiibo); saves still in a game folder move there on the first start. Test copies carry a
   `data_in_folder` marker and keep their data inside, so scripted runs never reach the player's saves.
2. Save states — *done (2026-11-19)*: F7/F8 (quick slot), `save`/`load` in `tools/play.py`. Guest-level, so they
   survive rebuilds: threads continue from their registers (recompiler resume points), state is written by each part
   of the runtime through one visitor (`runtime/state.c`), loading restarts the program with the state.
3. Trial playthrough: one agent plays one game (Ocarina of Time) through `tools/play.py`, to settle how agents play,
   what they report and how much it costs. Agents read up on the game first, play in a throwaway copy, run 2-3 at a
   time and only while the user is not playing.
4. First-hour playthroughs of all 8 games. Output: a list of crashes, stalls and rendering or audio errors per game,
   each checked against Azahar before it counts as a bug, and the GPU/DSP/service features each game uses.

## Stage B: complete emulation (1.0 needs all of it, used by the library or not)
Order inside the stage: what the playthroughs found first (crashes, stalls, then visible and audible errors), then
the rest of each list.
1. GPU (the whole PICA200):
   - geometry shaders — *done (2026-11-19)*: all three modes (point, variable primitive, fixed primitive) and b15
     after each invocation, as Azahar; checked by tests/pica_synth.py (no library game uses the last two);
   - procedural textures (complete), shadow maps and shadow textures, gas / fog volumes, min/max blending factors,
     logic ops, texture wrap modes 4-7, 16-bit colour blending, every texture and framebuffer format, every
     combiner and lighting LUT mode, the rest of the GX transfer flags (scaling, format conversion, linear/tiled);
   - immediate-mode drawing (0x232 index 15: vertices through 0x233-0x235) — *done (2026-11-19)*, found by the
     homebrew tests;
   - checked feature by feature against the software renderer and Azahar (test cases for unused ones). Homebrew
     test programs (`tests/homebrew.py`, devkitPro's examples and our own in `tests/homebrew/`) run in both and are
     compared screen by screen; and
     `tests/pica_synth.py` builds command lists, shaders and vertices from scratch and runs them through
     `build/gpu_replay` (software or Vulkan renderer).
2. Audio (the whole DSP): biquad filters, the remaining mixer details (aux buses, effects), every sample format and
   ADPCM detail, AAC decoding (DSP and the `mvd`/codec path games use).
3. Services: every service a retail game opens answers as a console would, or as a console without that hardware or
   network would: `am` (title, ticket and DLC queries), `cam` and `mic` (present, real ones in stage D), `soc`/`ac`/
   `frd`/`news`/`boss`/`cecd` reporting "no connection" cleanly, `mvd` (video), `qtm` (New 3DS head tracking: none),
   `ir:rst` (Circle Pad Pro / New 3DS), the default Mii and Mii import (`cfg`, Mii selector applet), software keyboard
   and other applets.
4. Performance pass on the heaviest games, guided by the playthroughs; GPU vertex shaders if the CPU path limits.

## Stage C: the finished tool
1. One shared `settings.toml` for controls and video; game files keep only game-specific defaults.
2. `3dsrecomp list`, clear progress and plain errors; the missing-function loop and `.3ds` dumps checked on real cases.
3. One command that runs every game's smoke test and the regression checks.
4. GameVault integration (asked for 2026-11-17; the user runs the GameVault Linux client):
   - `3dsrecomp package <dump>`: one self-contained archive per game (program, game data, launcher, icon), named
     with the dump's official English title (and release year) so GameVault matches it to IGDB; the archive name
     carries GameVault's Linux tag (check its docs first).
   - A folder watcher (systemd service or container) that packages new `.cia`/`.3ds` dumps once their build works
     and boots, and moves the dump to an archive folder: originals are kept, so later 3dsrecomp versions can rebuild.
5. Desktop integration: a launcher entry per game with its icon and title from the dump.
6. README: install, dumping your games, running, controls, settings; tested from a clean checkout.

## 1.0
- The GPU, DSP and services are complete (stage B), each feature checked against Azahar or the reference renderer.
- All 8 library games play through at least their first hour without crashes or obvious rendering or audio bugs.
- Any of the user's dumps goes from file to playing with one command.
- Settings, controls, save states and fullscreen are there; every game has a passing smoke test.
- Someone with a Linux PC and their own dumps can follow the README.

## Stage D: after 1.0
1. Real hardware:
   - Camera: the PC webcam (SDL camera) as the inner camera, webcam or a still image as the outer cameras.
   - Microphone: the PC microphone (SDL audio capture).
   - Wi-Fi, in steps: (1) internet for games: `soc:U` on host sockets and `ac` reporting a connection;
     (2) local wireless between two copies on the network (as Azahar); (3) online play through Pretendo Network
     (Mario Kart 7 is supported there): accounts, friends, `ssl:C`/`http:C`. Research first, then commit.
2. Amiibo library: the full list of amiibo (names, ids, series) from a public database as a data file; a searchable
   picker with favourites in the Esc menu; hotkeys to cycle favourites (F5/F6 tap/remove stay).
3. Save import/export: Checkpoint / JKSM formats (real 3DS) and Azahar.

## Optional extras
Stereoscopic 3D (anaglyph, side-by-side for 3D TVs and VR), cheats (Action Replay / Gateway codes), widescreen,
texture packs.

**Frame interpolation** (asked for 2026-11-17): smoother motion on high-refresh monitors by generating in-between
frames from two rendered ones (motion estimation on the GPU, or game-assisted: per-vertex motion from the two
frames' draws, which the renderer already sees). A big job with visible artifacts on 2D/UI layers, so it comes
with a per-game switch. Related, simpler: a frame-rate cap that is a game-speed limit (in the speed pass), and
per-game 60 fps patches for 30 fps games (with cheats).
