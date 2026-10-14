# SlideNX Reborn diagnostic NRO

This is a temporary foreground homebrew test, not the installable sysmodule.

## What it tests

- Whether the homebrew process can initialise the SD filesystem, audio output, and the `gpio` service.
- Whether the inherited raw GPIO IDs `0x0C` and `0x34` can be opened and read on the Switch OLED.
- Whether the same `source/mp3.c` playback implementation used by the sysmodule compiles and can play an MP3 in the foreground homebrew context.

## Build

From the repository root, in a devkitPro shell/container with `switch-dev` and `switch-mpg123` installed:

```sh
make -C diagnostic-nro clean all
```

Expected output: `diagnostic-nro/slidenx-diagnostic.nro`.

## Console test

1. Back up the SD card. Copy `slidenx-diagnostic.nro` to a convenient location such as `sd:/switch/slidenx-diagnostic.nro`.
2. Add your own short MP3s as `sd:/SlideNX/sounds/attach.mp3` and `sd:/SlideNX/sounds/detach.mp3`.
3. Launch the NRO through Homebrew Menu. If GPIO service access is denied in applet mode, retry using title takeover/full-memory homebrew mode.
4. Confirm that the screen reports SD/audio/GPIO status. Press A and B to test playback; physically attach/detach the Joy-Cons and record the raw values shown for each pin.
5. Exit with PLUS.

## Important interpretation

This NRO is useful as a safe first smoke test, but it does **not** validate the sysmodule's NPDM service permissions, automatic startup, background lifetime, or boot flags. If GPIO open fails only in the NRO, that can be a process-permission limitation and does not by itself prove the sysmodule will fail. Conversely, GPIO values changing here is evidence for the inherited pad mapping but still requires a later sysmodule test.

No NRO should be interpreted as confirmed working until tested on the actual OLED and the output/logs are recorded.
