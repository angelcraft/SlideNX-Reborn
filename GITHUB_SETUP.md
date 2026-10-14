# Build and test SlideNX Reborn 2.0

This branch builds a foreground homebrew application (NRO), not a sysmodule or NSP.

## GitHub Actions

Pushes to main, master, or rebuild branches run the build workflow. Open the Actions tab and download the SlideNX-Reborn-NRO artifact from the successful run summary. If the build fails, inspect the compiler log and diagnostic artifact.

## Install and test

1. Back up the SD card.
2. Copy slidenx.nro to sd:/switch/slidenx.nro.
3. Place both inputs at sd:/SlideNX/IN.mp3 and sd:/SlideNX/OUT.mp3.
4. Launch SlideNX from Homebrew Menu.
5. Press A to convert and verify the success status.
6. Press Y to install both outputs to LayeredFS.
7. Reboot and test the connect and disconnect sounds.
8. Reopen SlideNX and press X to restore the state captured before the first install.

The app does not install a sysmodule, poll GPIO, change boot flags, or alter Atmosphere configuration. A successful CI build still requires a real Switch test.
