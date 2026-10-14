# SlideNX Reborn 2.2 — Sound Studio

SlideNX Reborn is a foreground Nintendo Switch homebrew app (.nro), not a sysmodule.

## New in 2.1
- Choose any MP3 in sd:/SlideNX/sounds/ for connect and disconnect sounds.
- Keep original filenames; no renaming to IN.mp3 or OUT.mp3.
- Persist chosen filenames and gain to sd:/SlideNX/settings.ini.
- Preview up to 8 seconds of the selected connect or disconnect sound on the Switch.
- Adjust conversion gain from -12 dB to +6 dB in 1 dB steps.
- Convert to headerless signed PCM 16-bit little-endian, mono, 48 kHz.
- Convert and install remain separate actions.
- Restore the LayeredFS baseline saved before first installation.
- If sounds/ is empty, migrate legacy root IN.mp3 and OUT.mp3 into sounds/.

## Setup
1. Back up the SD card.
2. Copy slidenx.nro to sd:/switch/slidenx.nro.
3. Put MP3 files in sd:/SlideNX/sounds/.
4. Open SlideNX, choose connect/disconnect MP3s, and optionally adjust gain.
5. Preview either sound first if desired.
6. Press A on Convert selected sounds.
6. Press Y (or select Install) and reboot to test.

## Controls
- UP/DOWN: navigate main menu or picker.
- Select Preview connect or Preview disconnect and press A to hear an 8-second sample.
- A: choose a sound / activate selected menu item.
- LEFT/RIGHT: adjust gain by 1 dB while gain row is highlighted.
- B: refresh file list / cancel picker.
- Y: install converted RAWs.
- X: restore original LayeredFS state.
- PLUS: exit or cancel picker.

## Paths
Converted output: sd:/SlideNX/output/joint.raw and joint_out.raw.
LayeredFS target: sd:/atmosphere/contents/010000000000100C/romfs/sound/.

## Notes
MP3 files are limited to 16 MiB and 120 seconds, and must use a constant sample rate. Gain is applied during conversion; positive gain may clip peaks. The current UI is a controller-driven text interface; trim and sound packs are planned. A successful build does not replace testing on the Switch OLED. Keep sd:/SlideNX/backup/ until restore has been verified.

Based on SlideNX-Reborn and upstream SlideNX by jonyluke. See LICENSE for GPL v2. Vendored minimp3 notices remain in include/minimp3.h.
