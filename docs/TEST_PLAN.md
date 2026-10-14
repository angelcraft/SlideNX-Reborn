# Test matrix

| ID | Test | Expected result | Evidence |\n| N0 | Build diagnostic NRO with libnx only | `slidenx-diagnostic.nro` generated | GitHub Actions artifact |\n| N1 | Run diagnostic NRO on OLED | SD/audio/GPIO service status is displayed; A/B tones work if `audout` is available | Screenshot + status values |\n| N2 | Attach/detach each Joy-Con while diagnostic runs | Raw values for GPIO `0x0C` / `0x34` are observed and transitions recorded | Written L/R values and transition notes |
|---|---|---|---|
| B1 | Build in devkitPro shell with `switch-dev switch-mpg123` | `slidenx.nsp` generated, no unresolved mpg123/libnx symbols | Full compiler/linker log |
| P1 | Run packaging script | Correct `exefs.nsp` and `flags/boot2.flag` under title ID `420000000000534E` | `find dist -type f` listing |
| S1 | Boot on OLED, firmware 22.x, Atmosphère/Hekate | Console boots; Hekate Toolbox reports module running | Firmware, Atmosphère, Hekate versions + screenshot |
| S2 | Check `SlideNX.log` | SD, audio, and both GPIO sessions initialise | Log file |
| G1 | Attach/detach left Joy-Con | Exactly one custom event sound for each state change | Log transitions + observation |
| G2 | Attach/detach right Joy-Con | Same behavior as left | Log transitions + observation |
| A1 | `random=false`, fixed MP3s present | `attach.mp3` plays on attach, `detach.mp3` on detach | Log and observation |
| A2 | `random=true`, 2+ MP3s present | Random MP3 is selected; no crash with empty folder | Log and observation |
| A3 | Missing config, empty folder, nested folder, non-MP3 file | No divide-by-zero, out-of-bounds, or boot crash | Test each condition separately |
| R1 | Reboot 5 times, sleep/wake 3 times | No bootloop, unexpected audio loop, or persistent module failure | Test log |
| R2 | Remove title-ID folder and reboot | Original console behavior restored | Confirmation after rollback |

## Stop conditions

Stop testing and remove `atmosphere/contents/420000000000534E/` if the console fails to boot normally, audio output becomes stuck, Atmosphère reports a fatal error, or Hekate Toolbox marks the module as repeatedly crashing.
