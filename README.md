# SlideNX Reborn 2.4 — Sound Studio

SlideNX Reborn is a foreground Nintendo Switch homebrew app (.nro), not a sysmodule.

## New in 2.4
- Choose any MP3 in sd:/SlideNX/sounds/ for connect and disconnect sounds.
- Keep original filenames; no renaming to IN.mp3 or OUT.mp3.
- Persist chosen filenames and gain to sd:/SlideNX/settings.ini.
- Preview up to 8 seconds of the selected connect or disconnect sound on the Switch, using the same normalization approach as conversion.
- Adjust conversion gain from -12 dB to +6 dB in 1 dB steps.
- Automatically normalize connect and disconnect sounds independently using active-sample RMS, targeting -18 dBFS with a -3 dBFS sample-peak ceiling.
- Graphical SDL2 interface with menu highlighting, separate connect/disconnect cards, status panels, and a dedicated MP3 picker; controller controls and text fallback remain available.
- Cap automatic boost at +18 dB; manual gain remains available, but the peak ceiling takes priority.
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
7. Press Y (or select Install) and reboot to test.

## Controls
The graphical interface is designed for 1280×720 output and is controlled with the Switch controller. If SDL2 cannot initialize, the app falls back to the text interface.

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
MP3 files are limited to 16 MiB and 120 seconds, and must use a constant sample rate. Each MP3 is normalized separately: active-sample RMS targets -18 dBFS, sample peaks are capped at -3 dBFS, and automatic boost is limited to +18 dB. This is an RMS-based estimate rather than LUFS metering. Manual gain is applied afterward, but the peak ceiling takes priority. Normalization reduces output-level clipping risk; it cannot repair distortion already present in the source MP3. The current UI is a controller-driven text interface; trim and sound packs are planned. A successful build does not replace testing on the Switch OLED. Keep sd:/SlideNX/backup/ until restore has been verified.

Based on SlideNX-Reborn and upstream SlideNX by jonyluke. See LICENSE for GPL v2. Vendored minimp3 notices remain in include/minimp3.h.
