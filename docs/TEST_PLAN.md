# SlideNX Reborn 2.2 test plan
1. Back up SD.
2. Put at least two differently named short MP3 files in sd:/SlideNX/sounds/.
3. Launch the NRO and confirm it remains open.
4. Select connect and disconnect files and confirm selections persist after restart.
5. Preview both selections and confirm audio plays and returns to the menu.
6. Confirm selection never renames source files.
7. If the sounds folder is empty, verify migration of legacy root IN.mp3 and OUT.mp3.
8. Convert at 0 dB; check output RAWs and approximate 96,000 bytes per second duration.
9. Repeat at +3 dB; confirm size stays constant and audio gain changes.
10. Confirm conversion alone does not alter LayeredFS targets.
11. Install, reboot, test both sounds, then restore with X.
Do not describe the release as hardware-verified until tested on the target Switch OLED.
