# SlideNX Reborn 2.0 — test plan

## Build checks

- GitHub Actions compiles slidenx.nro with devkitPro.
- Artifact exists and is non-empty.
- Source tree no longer packages the old sysmodule/NSP or GPIO diagnostic app.

## Switch smoke test

1. Back up the SD card.
2. Copy slidenx.nro to sd:/switch/slidenx.nro.
3. Launch from Homebrew Menu and confirm the menu is visible.
4. Exit with Plus; repeat launch/exit at least three times.

## Conversion test

1. Put short known-good MP3s at sd:/SlideNX/IN.mp3 and sd:/SlideNX/OUT.mp3.
2. Press A and confirm success.
3. Confirm both files exist in sd:/SlideNX/output/.
4. For mono PCM 16-bit at 48 kHz, output size should be approximately 96,000 bytes per second of audio.
5. Confirm conversion alone did not change the LayeredFS sound files.

## LayeredFS install and rollback

1. Confirm the SD backup exists.
2. Press Y and check that both target RAW files were installed.
3. Reboot the console and test both sounds.
4. Press X to restore the baseline.
5. Check the previous override files were restored, or that files are removed if no override existed before first install.

## Failure behavior

- Missing inputs should show a status message and not alter target files.
- A decoding failure should not start installation.
- An install failure should attempt to restore the files that were present immediately before that install.
- Do not call the app hardware-verified until all tests pass on the target Switch OLED.
