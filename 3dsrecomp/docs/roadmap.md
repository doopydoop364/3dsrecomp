# Roadmap to 3dsrecomp 1.0

Agreed with the user on 2026-11-16. Status is kept current as work lands; the phase details for 1-8 of the general
tool are in `general_tool_design.md` section 10.

## Done
- Robobot fully playable (Vulkan renderer, audio, amiibo, PC settings, profiler).
- Phases 0-5 of the general tool: `3dsrecomp build|run <dump>`, extraction with seeds, discovery from the entry
  point, cached build stages; Mario Kart 7, Ocarina of Time 3D and Xenoblade Chronicles 3D boot and play.

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

## 3. Updates and DLC — *done (2026-11-19), waiting for a real update dump to test end to end*
`3dsrecomp build|run` take the game plus update (0004000E) and DLC (0004008C) dumps, given together or lying in the
same folder. The update's ExHeader and code replace the program before discovery (base kept in `game/base/`); its
RomFS is served as SelfNCCH type 5 (the game layers it over the base RomFS itself, as on a console); DLC contents are
served through the content archive 0x2345678A by content index. Design and findings: `general_tool_design.md`
section 12. Smash's DLC (139 contents) is unpacked, but its contents are only ownership markers: the fighters come
with the update, which is not in the library, so the base program never asks for DLC. `am:app` is not implemented
(no library game opens it).

## 4. GPU features
1. Geometry shaders — *point mode done (2026-11-17)*; variable-primitive and subdivision modes still log and skip.
2. Speed: an overlap-aware surface cache — *done (2026-11-19)*. Games reuse one block of VRAM for render targets of
   different sizes (MH4U: two overlapping 256x512 targets, a bloom chain inside one, a depth buffer at five sizes);
   these surfaces now coexist and hand over overlapping tiles on the GPU. tests/bench.sh: MH4U 10.4 -> 6.2 ms/frame,
   Xenoblade 8.6 -> 4.2. Design: `pc_experience_design.md`, "Overlapping surfaces".
3. The rest of the PICA200, driven by the feature audit over all 8 games first, then complete: procedural textures,
   shadow maps, gas (fog volumes), min/max blending factors, logic ops, wrap modes 4-7, 16-bit colour blending.

## 5. Audio
DSP effects (biquad filters and the remaining mixer details), AAC decoding where games use it.

## 6. Services
- A default Mii, so games that need one (Mario Kart 7's notice) work; importing Miis from a QR code or a file.
- Whatever the newly booting games turn up.

## 7. Real hardware
- Camera: the PC webcam (SDL camera) as the inner camera, webcam or a still image as the outer cameras.
- Microphone: the PC microphone (SDL audio capture).
- Wi-Fi, in steps: (1) internet for games: `soc:U` on host sockets and `ac` reporting a connection;
  (2) local wireless between two copies on the network (as Azahar); (3) online play through Pretendo Network
  (Mario Kart 7 is supported there): accounts, friends, `ssl:C`/`http:C`. Research first, then commit.

## 8. Phase 8: a finished tool
- One shared `settings.toml` for controls and video; game files keep only game-specific defaults.
- Save states.
- Amiibo library: the full list of amiibo (names, ids, series) from a public database as a data file; a searchable
  picker with favourites in the Esc menu; hotkeys to cycle favourites (F5/F6 tap/remove stay).
- Save import/export: Checkpoint / JKSM formats (real 3DS) and Azahar.
- Performance pass on the heaviest games (Smash, Xenoblade, Pokémon); GPU vertex shaders if the CPU path limits.
- `3dsrecomp list`, clear progress and plain errors; the missing-function loop and `.3ds` dumps checked on real cases.
- One command that runs every game's smoke test and the regression checks.
- Desktop integration: a launcher entry per game with its icon and title from the dump.
- **GameVault integration** (asked for 2026-11-17; the user runs the GameVault Linux client):
  - `3dsrecomp package <dump>`: one self-contained archive per game (program, game data, launcher, icon), named
    with the dump's official English title (and release year) so GameVault matches it to IGDB, which has the 3DS
    as a platform, for its metadata; the archive name carries GameVault's Linux tag (check its docs first).
  - A folder watcher (systemd service or container) that packages new `.cia`/`.3ds` dumps once their build works
    and boots, and moves the dump to an archive folder: originals are kept, so later 3dsrecomp versions can rebuild.
  - Saves outside the game folder (`~/.local/share/3dsrecomp/<title id>/`), so reinstalling or updating a game
    from GameVault keeps them.
- README: install, dumping your games, running, controls, settings; tested from a clean checkout.

## 9. Optional extras
Stereoscopic 3D (anaglyph, side-by-side for 3D TVs and VR), cheats (Action Replay / Gateway codes), widescreen,
texture packs.

**Frame interpolation** (asked for 2026-11-17): smoother motion on high-refresh monitors by generating in-between
frames from two rendered ones (motion estimation on the GPU, or game-assisted: per-vertex motion from the two
frames' draws, which the renderer already sees). A big job with visible artifacts on 2D/UI layers, so it comes
with a per-game switch. Related, simpler: a frame-rate cap that is a game-speed limit (in the speed pass), and
per-game 60 fps patches for 30 fps games (with cheats).

## 1.0
- All 8 library games boot and play through at least their first hour without crashes or obvious rendering bugs.
- Any of the user's dumps goes from file to playing with one command.
- Settings, controls, save states and fullscreen are there; every game has a passing smoke test.
- Someone with a Linux PC and their own dumps can follow the README.
