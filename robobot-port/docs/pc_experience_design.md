# PC experience layer: design

## Core principle

The game always believes it's running on a 3DS. Every PC feature lives in a host layer around the
game: how frames are shown, how input is produced, how the user configures things. The recompiled
code and the HLE services stay accurate. This keeps the virtual clock and the regression checks
valid, and every enhancement can be switched off to check against the original.

Three tiers:

- **Universal:** works for any game with no per-game knowledge. Window, layouts, resolution,
  filtering, controls.
- **Per-game, data only:** settings in `games/<title id>.toml`. Touch hotspots, default layout,
  prompts to hide.
- **Per-game, code hooks:** replace a guest function with native C. Widescreen, skipping intros,
  text changes. These are opt-in and riskiest.

## Components

### 1. Frontend and settings
- SDL3 window: windowed, borderless fullscreen and exclusive fullscreen, plus a resizable window
  with integer or fit scaling.
- Settings live in a global `settings.toml`, overridden by the per-game file, then by command-line flags.
- An overlay menu on Escape, drawn by the host on top of the game: Resume, Video, Controls, Audio,
  Layout, Quit. The game is paused while it's open, which the virtual clock makes trivial.

### 2. Screen compositor
- It takes the two finished screen images (top 400×240, bottom 320×240) and arranges them.
- Layouts:
  - Stacked: like the console.
  - Large top: the bottom screen as a corner overlay, with a hotkey to show, hide or swap it.
  - Side by side.
  - Top only: the bottom screen pops up when touched or on a key.
- Swapping the screens is a hotkey.
- A good default for Robobot: large top screen with the bottom screen hidden, since it's mostly menus.

### 3. Renderer upgrades (after Vulkan)
- Internal resolution scale: render 3D at 2x, 4x or the native window size.
- Main design challenge: the game moves its render targets around with display transfers and
  texture copies, and the CPU sometimes reads that memory.
- This needs a surface cache like Citra's rasteriser cache. It tracks which VRAM ranges are GPU
  render targets, keeps them in high resolution on the GPU, and writes them back at native
  resolution only when the CPU or a copy needs the real bytes.
- This is the biggest single piece of work in the plan, so design the Vulkan renderer around it
  from the start.
- Texture filtering: linear or anisotropic regardless of what the game asks for, as a per-game
  toggle because it can break pixel-art UI.
- Anti-aliasing: MSAA on the 3D render targets, or FXAA as a post pass.
- Sharp 2D: keep UI layers nearest-neighbour at integer scale where possible.

### 4. Input
- An action-mapping layer: physical key or button, then an action, then 3DS buttons, circle pad or touch.
- Gamepad (SDL): A/B/X/Y, shoulders, left stick to the circle pad, the d-pad, Start/Select, plus an
  optional right stick or triggers for ZL/ZR games.
- Keyboard: default WASD or arrows, rebindable in the overlay.
- Touch:
  - The mouse on the bottom screen acts as the stylus, wherever the compositor puts that screen.
  - Touch hotspots in the per-game file, for example `F1 = tap(160, 200)`. They make the touch
    screen optional and suit games like Robobot with simple touch menus.
- Later: motion controls via the gyro of a gamepad (DualSense, Switch Pro), for games that enable
  the accelerometer or gyroscope.

### 5. Per-game code hooks
- Because the code is recompiled, any guest function can be replaced or wrapped. The per-game file
  lists them, for example `[hooks] 0x123456 = "widescreen_projection"`, and the recompiler or the
  dispatch table routes the call to native C.
- Uses:
  - Widescreen: patch the game's projection setup to a wider aspect ratio, then reposition 2D
    elements that assume 400 pixels. Truly per-game, so it needs REA analysis per title.
  - Skip intros and logos: jump past the logo sequence after the first boot.
  - Hide console prompts: suppress the "Press HOME" draw or patch the message text.
  - Glyph replacement: swap button-prompt textures for Xbox or keyboard icons by overriding RomFS
    files. That's data only and easy.

### 6. Conveniences
- Saves in a clear folder, with an option to point it at a cloud-synced directory.
- Quick save and load of the full guest state. The runtime owns all memory and CPU state, so
  snapshots are realistic.
- Fast-forward on a key: the virtual clock with pacing turned off.
- Screenshots on F12, which is mostly built already with `RB_SHOT`.

### 7. Frame rate beyond 60 (stretch goal)
- Game logic is fixed at 60 Hz. Options are frame interpolation, which is generic but adds latency
  and artefacts, or per-game patches that double the logic rate, which is risky.
- Treat this as the last item. Many 3DS games run at 30 fps internally, so a steady game-native
  rate already feels good.

## When to build each part

| Stage | Build | Why then |
|---|---|---|
| With Phase 5 input (next after the title fix) | settings file, window and fullscreen, gamepad, keyboard, mouse touch, layout compositor (stacked, large top, top only) | Input is needed anyway, and the compositor works on the current software renderer. |
| While designing the Vulkan renderer | surface-cache architecture | Retrofitting resolution scaling later is painful. Plan for it from the start, even if it ships later. |
| After Vulkan works | resolution scale, filtering, AA | These depend on the GPU renderer. |
| After Robobot plays end to end | overlay menu, hotspots, hooks (skip intro, hide prompts, glyphs), save states | Needs a playable game to tune against. |
| With the 3dsrecomp general tool | split universal defaults from per-game profiles, a game library with per-game settings | That's when a second game proves what's generic. |
| Last | widescreen per game, beyond 60 fps | Per-title research and the most risk. |

## Rules

- Every enhancement can be switched off. "Original" mode must match the reference emulator, and
  that stays the regression check.
- Enhancements never change the game's logic timing. The virtual clock is untouched.
- Per-game data before per-game code. Only use hooks when data can't do it.
- Each feature gets a screenshot or recording check, like the renderer milestones.

## Vulkan renderer: surface cache from the start

The Vulkan renderer is designed around a surface cache even if resolution scaling ships later.
Constraints it must meet, so scaling can be switched on without restructuring:

- Every colour and depth buffer the PICA renders into is a cached surface keyed by its physical
  address range, format and size, with a scale factor (1x in "original" mode).
- GX operations (display transfer, texture copy, memory fill, DMA) and texture sampling look up
  surfaces first and operate GPU to GPU when source and destination are cached.
- Guest memory is the truth at native resolution. A surface is written back (downscaled) only when
  the CPU or an uncached copy needs those bytes; CPU writes into a cached range invalidate it.
  Detecting CPU access needs page protection on VRAM/FCRAM ranges or explicit flush points
  (GSP cache flush/invalidate commands, display transfer sources).
- The software renderer stays as the reference: in original mode, Vulkan output read back at 1x
  must match it (and the reference emulator) screenshot for screenshot.
