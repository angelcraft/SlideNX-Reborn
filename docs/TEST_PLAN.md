# SlideNX Reborn 2.7 hardware test plan

## Core behavior
1. Back up the SD card before testing.
2. Put at least two differently named short MP3 files in `sd:/SlideNX/sounds/`.
3. Launch the NRO; confirm the graphical interface opens and navigation remains responsive.
4. Select connect and disconnect files; confirm filenames and gain persist after closing/relaunching SlideNX.
5. Preview both selections and confirm audio plays without dropouts and returns to the menu.
6. Confirm selecting a sound never renames or overwrites the source MP3.
7. If `sounds/` is empty, verify legacy `IN.mp3` and `OUT.mp3` migration.
8. Convert at 0 dB; check output RAWs and approximate 96,000 bytes per second of audio.
9. Repeat at +3 dB; confirm output size stays duration-dependent and sound level changes.
10. Confirm conversion alone does not alter LayeredFS targets.
11. Install converted RAWs, verify both target files exist, then use restore and verify the original backup is restored.
12. Verify settings backup recovery and stale-output validation when source or conversion settings change.

## Restart action
13. Navigate to **Restart Switch** and press A; verify the confirmation screen clearly warns that the console will reboot.
14. Press B on the confirmation screen; confirm SlideNX returns to the menu without restarting.
15. Open the confirmation screen again and press PLUS; confirm it cancels without restarting.
16. Only after installation, select Restart Switch and confirm with A. Verify the console actually reboots and both sounds take effect afterward.
17. If the reboot request is rejected by the current homebrew launch context, verify an error code is shown and the application does not pretend the restart succeeded.

Do not describe a release as hardware-verified until the reboot and audio behavior have been tested on the target Switch OLED.
