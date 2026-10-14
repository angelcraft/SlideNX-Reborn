# SlideNX Reborn 2.1 test plan
1. Back up SD.
2. Put at least two differently named short MP3 files in sd:/SlideNX/sounds/.
3. Launch the NRO and confirm it remains open.
4. Select connect and disconnect files and confirm selections persist after restart.
5. Confirm selection never renames source files.
6. If the sounds folder is empty, verify migration of legacy root IN.mp3 and OUT.mp3.
7. Convert at 0 dB; check output RAWs and approximate 96,000 bytes per second duration.
8. Repeat at +3 dB; confirm size stays constant and audio gain changes.
9. Confirm conversion alone does not alter LayeredFS targets.
10. Install, reboot, test both sounds, then restore with X.
Do not describe the release as hardware-verified until tested on the target Switch OLED.
