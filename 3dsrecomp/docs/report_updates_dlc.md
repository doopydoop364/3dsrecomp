# Report: updates and DLC, loose ends (2026-11-19)

Every change is generic (no per-game code). Design and findings: `general_tool_design.md` section 12.

## Updates and DLC
- `3dsrecomp build|run <game> [update] [dlc]`, or the update / DLC dump in the game dump's folder (found by title id):
  added to `work/<id>/game/` once, a newer version replaces it. The update's program is what gets recompiled (the
  base program and what runs had learned about it move to `game/base/`); its RomFS is served as SelfNCCH type 5;
  DLC contents are served through archive 0x2345678A by content index.
- Checked: DLC unpacking (Smash, 139 contents); the update path with stand-ins (Ocarina's dump applied as an update
  on a scratch copy; Smash with its RomFS served as the update RomFS reaches character select as before).
- Not checked: a real update, and DLC as a game reads it. Smash's DLC contents are only ownership markers and the
  fighters come with Smash's update (0004000E000EDF00), which is not in the library; without it the game never asks
  for DLC. `am:app` is not implemented (no library game opens it).

## Loose ends
- **APT Wrap / Unwrap**: AES-CCM (`runtime/aes.c`, checked against pycryptodome on 200 random cases) with the
  key from `aes_keys.txt` (`slot0x31KeyN`) in the system folder, else an all-zero key, as Azahar.
- **Tomodachi Life's demo-data question**: Azahar (installed from Flathub, run on a headless display with a decrypted
  CXI) shows the same "demo-version data has been found" on a fresh console, so it is not a 3dsrecomp bug. On the
  way: save data asked for on a game card (archives 0x567890B2/B4, media type 2) now answers "game card not
  inserted" as Azahar does, unless the game was extracted from a cartridge dump (`game/cartridge`).
- **StreetPass write-back**: already done (OpenAndWrite stores what the game writes in `game/cecd/`).
- System archives are served from their exact IVFC level-3 size (was: file size minus 0x1000).

## Regression
Robobot regress 12/12. Smoke tests of all 8 other games pass; Ocarina, Xenoblade, MH4U and Pokémon X screenshots are
pixel-identical to before. Mario Kart 7 and Animal Crossing differ, but two runs of the same build differ as much
(their smoke runs are not deterministic at those frames); Smash differs by its clock; Tomodachi Life by the changes
above.
