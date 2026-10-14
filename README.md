# SlideNX Reborn 2.0 — Sound Converter & LayeredFS Installer

SlideNX Reborn is a foreground Nintendo Switch homebrew application (.nro). It is not a sysmodule and does not poll Joy-Con GPIO or run in the background.

Its purpose is to generate and install the connect and disconnect sound replacements tested with LayeredFS.

## Features

- Reads IN.mp3 and OUT.mp3 from the SD card.
- Decodes MP3 audio with the vendored minimp3 library.
- Converts to headerless signed PCM, 16-bit little-endian, mono, 48,000 Hz.
- Generates joint.raw and joint_out.raw.
- Installs both outputs to the known overlayDisp LayeredFS path.
- Captures a baseline backup before the first installation.
- Supports restoring the previous override, or removing overrides that did not exist before SlideNX was used.
- Converting audio alone never changes the LayeredFS target files.

## SD card setup

Back up your SD card before the first test.

Copy slidenx.nro to:

    sd:/switch/slidenx.nro

Create sd:/SlideNX/ and place both MP3 files there:

- sd:/SlideNX/IN.mp3 — Joy-Con connect sound.
- sd:/SlideNX/OUT.mp3 — Joy-Con disconnect sound.

Open SlideNX from Homebrew Menu.

## Controls

- A — Convert both MP3 files.
- Y — Install both converted RAW files to LayeredFS.
- X — Restore the baseline state saved before the first installation.
- B — Refresh file status.
- Plus — Exit.

Converted files are stored at:

- sd:/SlideNX/output/joint.raw
- sd:/SlideNX/output/joint_out.raw

LayeredFS target:

- sd:/atmosphere/contents/010000000000100C/romfs/sound/joint.raw
- sd:/atmosphere/contents/010000000000100C/romfs/sound/joint_out.raw

Reboot after installing so the system reloads the override.

## Safety and limits

- This rewrite must be compiled and then tested on hardware; a successful build is not a guarantee of runtime behavior.
- MP3 input is limited to 16 MiB and 120 seconds of decoded source audio.
- Sample rate must remain constant across the MP3.
- Audio is downmixed to mono and linearly resampled to 48 kHz.
- The app changes only the two specified LayeredFS sound files. It does not change Atmosphere configuration, boot flags, GPIO, or system modules.
- Keep sd:/SlideNX/backup/ until restoration has been tested. It stores the state of these two LayeredFS files, not a full SD backup.

## Build

Install devkitPro with the Switch development toolchain (switch-dev). From the repository root, run:

    make clean
    make

The output is slidenx.nro. GitHub Actions builds the same application and provides it as an artifact.

## Attribution and license

This project is based on the SlideNX-Reborn repository and the upstream SlideNX work by jonyluke. The project uses GNU GPL v2; see LICENSE. The vendored minimp3 header retains its upstream license and attribution notices.
