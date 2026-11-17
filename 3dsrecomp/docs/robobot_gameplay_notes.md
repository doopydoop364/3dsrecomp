# Kirby: Planet Robobot — gameplay notes for scripting and bug triage

Researched before judging "is this a bug?" from screenshots. Sources: WiKirby (Patched Plains, Patched Plains - Stage 1),
Ludo.guide (stage 1-1 walkthrough, controls & copy abilities), Nintendo's electronic manual.

## Controls (default 3DS layout; port keys in parentheses)
- Control Pad / Circle Pad: walk; double-tap = dash (dash jumps go further). Down = crouch.
- A (L key): jump. A again / hold in mid-air = hover; hovering is unlimited, so tapping A forever floats to the top of the screen.
- B (K key): inhale / spit (air bullet while hovering). Down after inhaling = swallow / copy ability.
- Down + A: slide. Down, Down in mid-air: quick fall.
- X (I key): drop copy ability. Y (J key): use Assist Star item.
- L / R (Q/E): guard; with a direction = dodge; in mid-air = air dodge.
- Robobot Armor: A jump, B punch (mash B = slam), X resets specs.
- Earlier scripts used B as "jump": wrong, B inhales.

## How stages connect
- On the area hub, stages are entered through numbered doors (that is the only "door" use so far).
- Inside a stage, sections are linked by **tubes/pipes and Warp Stars**, not doors. 3D Warp Stars fly Kirby between the
  foreground and background planes.
- The end of a stage: the stage exit, then a timing mini-game that launches Kirby (a button press at the right moment).

## Patched Plains — Stage 1 in order (no Robobot Armor here; it first appears in Stage 2)
1. Grassy steps going right, stars from flowers, breakable metal drums. Sparky (Spark), Waddle Dees with spike bars
   (sticker 1), Sir Kibble (Cutter). Tutorial signs (e.g. the hover sign at a tall pipe wall: hover over it).
   The section ends at a tube / Warp Star to the next room.
2. Hostile locals and barrels: break a grey block to reveal a tube; inside, Blade Knight (Sword), sticker 2 behind crates.
   Continue right to the first 3D Warp Star.
3. Large tube, Galbo (Fire), another 3D Warp Star, a Bandana Waddle Dee throws an item, red Waddle Doo (Beam).
4. Clanky Woods chase: it fires missiles and leaps; running past it to the tube at the end is enough (defeating it gives
   a rare sticker).
5. Pipe, Maxim Tomato, pipe to the final room: Code Cube 1 hanging over the path, then the stage exit and mini-game.

## Not yet confirmed from sources
- Exact input to enter a tube (walking into it vs pressing a direction) and Warp Stars (touching them).
- What looks normal on screen at each transition; compare with real footage before calling a bug.

## Bottom screen during stages (manual, "Stage Screen")
- Current copy ability / specs, health, lives; **amiibo icon** (touch to use an amiibo; appears only after progressing
  some way through the story); Code Cubes found; stars; **Miiverse icon** (the green figures button, top right);
  current Assist Star item (touch to use). "Drop Ability" and "Pause" are buttons too.

## amiibo (manual, "amiibo Inhale" and "About amiibo")
- Unlocked after progressing through the game: the amiibo icon appears on the touch screen; touch it, then tap an amiibo
  and Kirby inhales it for an item and a copy ability (not while in the Robobot Armor; up to ten per stage).
- Kirby, Meta Knight and King Dedede amiibo give better items and special-design abilities; other series map to
  abilities (e.g. Smash Bros. Kirby; Meta Knight/Link/Marth Sword; King Dedede Hammer; Mario/Luigi Fire; Peach Parasol...).
- New 3DS reads amiibo under the touch screen; older models need the NFC Reader/Writer accessory.
- Port: nfc:u in runtime/nfc.c; amiibo .bin dumps in <game>/amiibo/, F5 taps the next one, F6 removes it.
  Verified in-game by the user (touch the icon, F5): Kirby inhaled the test Kirby amiibo.
