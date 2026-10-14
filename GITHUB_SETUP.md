# Build and test SlideNX Reborn 2.2

This branch builds SlideNX as a foreground homebrew application (.nro), not a sysmodule or NSP.

## GitHub Actions

The workflow builds the NRO and uploads it as the SlideNX-Reborn-NRO artifact. Open Actions and download the artifact from a successful run. If the build fails, inspect the compiler log and build diagnostics.

## Install

1. Back up the SD card.
2. Copy slidenx.nro to sd:/switch/slidenx.nro.
3. Place MP3 files in sd:/SlideNX/sounds/.
4. Launch SlideNX from Homebrew Menu.
5. Select connect and disconnect sounds from the file picker.
6. Optionally adjust conversion gain and preview each selected sound.
7. Press A on Convert selected sounds.
8. Press Y or select Install to LayeredFS.
9. Reboot to test both Joy-Con sounds.
10. Reopen SlideNX and press X to restore the baseline saved before the first installation.

The application does not install a sysmodule, poll GPIO, change boot flags, or alter Atmosphere configuration. Conversion does not install automatically. A successful CI build still requires a real Switch test.
