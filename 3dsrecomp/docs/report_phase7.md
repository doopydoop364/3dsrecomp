# Phase 7 report: CRO modules (2026-11-17 to 2026-11-18)

All eight games of the test library now start; every change is generic (games/<id>.toml holds only test scripts).

| Game | Code | Furthest point the smoke script reaches |
|---|---|---|
| Super Smash Bros. for 3DS | ARM + Thumb, 1 module ("menu", built in memory) | title, character select |
| Pokémon X | ARM + Thumb, 133 modules | language select, intro with the professor, character choice, name input |
| Animal Crossing: New Leaf Welcome amiibo | ARM + Thumb, 38 modules | title, New Save File, the train with Rover, name input |

## How it works
- **`ldr:ro`** (`runtime/ldr_ro.c`): static.crs, LoadCRO / LoadCRO_New, UnloadCRO, LinkCRO / UnlinkCRO, Shutdown with
  Azahar's rules, in our own code. Its writes into the main program's read-only import slots are allowed.
- **Position-independent module code** (`recomp/armrecomp.py --module`): every address module code computes is
  `A(offset)` = the module's current base + offset, so each module is compiled once and runs wherever and however
  often the game loads it (Pokémon X loads, unloads and reloads modules at reused addresses).
- **Discovery** in a module starts from its exports, hooks and relocations into code; in the main program, the
  functions static.crs exports or modules import anonymously are seeds, and the words ldr:ro writes are never
  folded into constants. All three games use only absolute relocations (type 2): data words, never instructions.
- **The run loop** covers modules: a module that loads without compiled code is saved to `work/<id>/cro/` (Smash
  builds its module in memory); code a module's analysis missed is noted as `work/<id>/cro/<name>.extra`. Either way
  `3dsrecomp run` rebuilds and restarts. `tests/converge.sh` does the same in the test copy.
- Also found on the way: finished threads' slots are reused (Pokémon X ends and creates threads as modules change);
  Thumb inline jump tables (armcc) dispatch to local labels.

## Open
- Each game was played only through its first minutes by script; longer play will find more (the run loop absorbs
  missing code automatically; other gaps will show as service or GPU issues).
- Animal Crossing and Pokémon X have not been benchmarked yet.
