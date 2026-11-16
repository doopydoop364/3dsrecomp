# Phase 1 recon: Kirby: Planet Robobot (CTR-P-AT3A, title 0004000000183600)

## Extraction
- Source: original CIA. The "-decrypted.cia" from decrypt_cia.py is NOT fully decrypted (seed crypto unsupported).
- NCCH flags 0000000101030020: crypto method 0x01 (KeyX 0x25) + seed crypto (0x20).
- Seed verified against NCCH header hash. Extractor: scratchpad/extract_seeded.py
- Output dir: scratchpad/extract/ (code.bin, code_decompressed.bin, exheader.bin, banner.bin, icon.bin, romfs.bin)

## Code layout (ExHeader, page-aligned, contiguous in code_decompressed.bin)
| Segment  | Address   | Size (file)  |
|----------|-----------|--------------|
| .text    | 0x100000  | 0x579000 (0x5784e0 used) |
| .rodata  | 0x679000  | 0x067000 (0x66530 used)  |
| .data    | 0x6e0000  | 0x02b000 (0x2aaf0 used)  |
| .bss     | 0x70b000  | 0x12d8c       |
- Code was BLZ-compressed (0x315db0 -> 0x60b000). No CRO modules referenced so far.

## REA / Ghidra (ARM:LE:32:v7, raw-binary, entry 0x100000)
- 34,660 functions, 19,850 defined strings.
- Binary is stripped: only 3 functions have real names.
- NOTE: Ghidra's generic ARM v7 language is used. The 3DS ARM11 is ARMv6K. Mixed ARM/Thumb
  is handled by Ghidra analysis, but thumb-entry detection should be double-checked.

## OS services referenced (string table near 0x679028..0x6dab78)
ndm:u, fs:USER, dsp::DSP, ir:USER, nwm::UDS, y2r:u, cfg:u/i/s, hid:USER, APT:U/A,
gsp::Gpu, ptm:sysm/s/gets/u/play
=> Services that need native replacements: fs, gsp, dsp, hid, cfg, apt, ptm, y2r (video/camera YUV->RGB).
   ndm/ir/nwm (network, IR, local wireless) can be stubbed.

## File paths / formats seen
- rom:/snd/Torte.bcsar (CTR sound archive), lyt/%s.arc (layout archives), *.ctpk, .bclim textures
- map/Step/%s/%s/%s.dat, map/MelonStep/..., map/Confetti/... (stage data)
- fdg/Archive.dat
- $CFLRES:/CFL_Res.dat, $CFLDB:/CFL_DB.dat (Mii face library data)
- Saves: data:/savedata.dat, extdata:/extsavedata.dat, $coin:/gamecoin.dat (play coins)

## Next
1. List RomFS (parse IVFC -> level 3) and catalog formats by extension.
2. Map every `svc` instruction (SVC numbers) and their callers.
3. Verify Thumb/ARM function discovery; export function list for the recompiler.

---
# Phase 1 completion

## RomFS (4,018 files, 650.6 MB; list in romfs_files.tsv as offset<TAB>size<TAB>path, offset relative to romfs.bin)
| Ext | Files | MB | Notes |
|---|---|---|---|
| .cmp | 3052 | 150 | Compressed wrapper, LZ11 (header byte 0x11). Inner types: bch 1982, arc 424, bin 340, ptcl 281, bcfnt 25 |
| .bcstm | 181 | 379 | Streamed music (CTR stream format) |
| .dat | 388 | 36 | Stage data (map/...) and others |
| .moflex | 2 | 70 | Mobiclip video: needs a Mobiclip decoder (ffmpeg has one) |
| .msbt/.msbp | 271 | 2 | Text and message project |
| .bcsar | 1 | 11 | Sound archive (snd/Torte.bcsar) |
| .cmpbin, .shbin, .ctpk, .txt, .icn | ~125 | ~2 | shbin = PICA200 shader binaries (only 4) |
Top-level dirs by size: snd 390, gfx 107, mov 70, map 36, lyt 28, mint 9, ptcl 8, msg 2, font 1.

Takeaways: models are BCH (CTR native), most assets sit behind a simple LZ11 layer, and only 4 shader
binaries exist, so the PICA200 shader translation surface is small.

## SVC surface (svc_scan.py): 29 distinct SVCs, each appears exactly once = 29 SDK wrapper stubs
ControlMemory, QueryMemory, ExitProcess, CreateThread, ExitThread, SleepThread, GetThreadPriority,
CreateMutex, ReleaseMutex, CreateEvent, SignalEvent, ClearEvent, CreateMemoryBlock, MapMemoryBlock,
UnmapMemoryBlock, CreateAddressArbiter, ArbitrateAddress, CloseHandle, WaitSynchronization1/N,
DuplicateHandle, GetSystemTick, GetHandleInfo, GetProcessInfo, ConnectToPort, SendSyncRequest,
GetProcessId, GetThreadId, Break.
=> The whole kernel interface is 29 small functions; replace those natively. SendSyncRequest (0x32) is the
   single IPC choke point; all service calls (fs/gsp/dsp/hid/...) pass through it.

## ARM vs Thumb
~27,072 ARM `push {..,lr}` prologues vs <=717 possible Thumb ones (includes false hits). Code is
essentially all ARM mode. Ghidra's generic ARM language handles this; the recompiler can start ARM-only
and treat Thumb as a later addition.

## Known caveat
Ghidra used ARMv7 language, game CPU is ARMv6K. Differences are small for decoding, but check VFP/exclusive-access
instructions when writing the recompiler's instruction semantics.
