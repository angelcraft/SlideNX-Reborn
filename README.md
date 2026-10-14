# SlideNX Reborn (candidate port)

A maintenance-oriented adaptation of [jonyluke/SlideNX](https://github.com/jonyluke/SlideNX), a Nintendo Switch sysmodule intended to play a custom sound when a Joy-Con attaches/detaches.

**Status:** source adaptation only; not yet compiled or tested on physical hardware. It is not a verified drop-in release. The raw GPIO IDs (`0x0C` and `0x34`) are inherited from upstream and are the main hardware-specific unknown on Switch OLED.

## Changes in this candidate

- Replaces reliance on `appletMainLoop()` with a sysmodule-style worker loop and 20 ms polling.
- Checks service calls and logs startup, GPIO transitions, and playback errors to `sdmc:/SlideNX/SlideNX.log` where the SD filesystem is accessible.
- Safely defaults to `random=false` when `config.ini` is missing or unreadable.
- Enumerates only regular `.mp3` files, ignores dot entries/directories, caps random list at 96 files, and avoids modulo-by-zero.
- Fixes the original `mpg123_getformat()` pointer-size mismatch: `long *` and `int *` must not point into 32-bit/8-bit variables.
- Forces 48 kHz stereo signed-16-bit PCM for libnx audout and passes the number of decoded bytes rather than claiming a full buffer is valid audio.
- Uses one aligned PCM buffer synchronously; simpler and less prone to buffer ownership/accounting mistakes than the original double-buffer loop.
- Restricts declared service access to `fsp-srv`, `gpio`, and `audout:u`; filesystem rights request the SD-card access bit only.

## Hardware / firmware assumptions

Target: Switch OLED, Horizon OS 22.x, Atmosphère + Hekate. The source calls the same GPIO pad identifiers as upstream SlideNX. I cannot confirm those identifiers behave identically on OLED without running the module on the actual console.

For Horizon 22.0.0, official Atmosphère 1.11.0 added support; 1.11.1 added basic 22.1.0 support; 1.11.2 added basic 22.5.0 support. Match your actual firmware to the appropriate Atmosphère build before testing. See the [official release notes](https://github.com/Atmosphere-NX/Atmosphere/releases).

## Cloud build with GitHub Actions

The workflow deliberately builds a small **diagnostic NRO first** on pushes and pull requests. Download `SlideNX-Reborn-Diagnostic-NRO` from the workflow run's **Summary → Artifacts** section, copy the NRO to `sd:/switch/`, and use it to check whether the inherited GPIO IDs `0x0C`/`0x34` can be opened/read and whether the audio output service plays short generated tones.

Only after you have tested that diagnostic on the OLED should you manually dispatch the workflow with target `sysmodule`. That run builds the diagnostic NRO again first, then attempts to build/package the NSP and uploads `SlideNX-Reborn-SD-package` if successful. Current environment note: devkitPro's package server has returned HTTP 403 to GitHub-hosted Actions runners when downloading `switch-mpg123`; the NRO diagnostic avoids this external MP3 dependency, but the sysmodule build still needs it.

A passing NRO build is not a passing hardware test, and a passing GPIO/audio diagnostic does not validate sysmodule NPDM permissions, startup, or stability. Test on emuMMC or with an SD backup before installing a sysmodule. No workflow modifies the console automatically.

## Build prerequisites

Use the devkitPro MSYS2 shell (Windows) or a shell configured for devkitPro (Linux/macOS), with a current devkitA64 and libnx installed.

```sh
dkp-pacman -S switch-dev
```

Then from the project folder:

```sh
./build_and_package.sh
```

The build script runs `make clean`, builds `slidenx.nsp`, and stages a `dist/` folder. If you prefer manual packaging, run `make clean && make`, then copy `slidenx.nsp` to `atmosphere/contents/420000000000000F/exefs.nsp` and create an empty `atmosphere/contents/420000000000000F/flags/boot2.flag`.

## SD card layout

Copy the **contents** of `dist/` to the SD card root. Expected result:

```text
SD:/
├── atmosphere/
│   └── contents/
│       └── 420000000000000F/
│           ├── exefs.nsp
│           └── flags/
│               └── boot2.flag
└── SlideNX/
    ├── config.ini
    └── sounds/
        ├── attach.mp3
        └── detach.mp3
```

The sample package does not include MP3 audio assets. Add your own MP3 files under `SlideNX/sounds/`. In fixed mode, name them `attach.mp3` and `detach.mp3`. Set `random=true` in `SlideNX/config.ini` for random selection; every regular `.mp3` in the folder becomes eligible.

## Safe test / rollback plan

1. **Back up the SD card** (or use emuMMC for first testing, if available). Do not replace a working Atmosphère/Hekate installation or alter Nintendo system files.
2. Compile on the PC. Confirm the build produced `slidenx.nsp` and `dist/atmosphere/contents/420000000000000F/exefs.nsp`.
3. Copy `dist/` to the SD root while the console is powered off. Keep the original `SlideNX` backup separate.
4. Boot and check Hekate Toolbox (sysmodule/background services) to see whether title ID `420000000000000F` is running. If it fails to start, remove that title-ID folder from the SD card before rebooting.
5. Inspect `SlideNX/SlideNX.log`. Confirm it reports SD mounted, audio runtime ready, and GPIO sessions ready. If GPIO session opening fails, treat the raw pin mapping as incompatible until investigated.
6. Test one Joy-Con at a time by attaching/detaching it. Note that this code preserves the upstream assumption `GPIO low = attached`; the direction may need reversing if the OLED signals the opposite polarity.
7. Test fixed mode with short clips first, then random mode with at least two MP3s. Verify audio volume, timing, and behavior while navigating the HOME Menu and resuming from sleep.
8. Reboot several times and test both Joy-Cons, handheld mode, docked mode if relevant, and sleep/wake. Do not treat a single successful boot as proof of stability.
9. Roll back by powering off and deleting `atmosphere/contents/420000000000000F/` and `SlideNX/` from the SD card. This does not uninstall or modify Atmosphère itself.

## Known limitations / open questions

- No build or Switch runtime test has been performed in this environment because devkitPro/libnx/mpg123 for Switch are not installed here.
- GPIO pad IDs are undocumented raw values carried forward from the original project; OLED compatibility must be demonstrated.
- Sound playback is synchronous. Very rapid repeated attachment changes during a long clip may be missed.
- The original project's limitation remains: this plays a custom sound but does not mute Nintendo's original attachment sound.
- This implementation has not been tested for every 22.x point release, sysmodule resource pressure, or sleep/wake edge cases.

## Attribution and license

Based on SlideNX by jonyluke, released under GNU GPL v2. See `LICENSE`. The changes in this candidate are provided under the same license.
