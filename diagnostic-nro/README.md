# SlideNX Reborn diagnostic NRO

This is a foreground homebrew smoke test, not the installable sysmodule. It deliberately depends only on libnx; it does not need the external `switch-mpg123` package that has been blocked by HTTP 403 responses in GitHub Actions.

## What it tests

- Whether the homebrew process can initialise the SD filesystem, audio output, and the `gpio` service.
- Whether the inherited raw GPIO IDs `0x0C` and `0x34` can be opened and read on the Switch OLED.
- Whether `audout` can play a generated short PCM tone (A = 660 Hz, B = 440 Hz).

## Build

With the official `devkitpro/devkita64` environment or a local devkitPro installation with `switch-dev` installed:

```sh
make -C diagnostic-nro clean all
```

Expected output: `diagnostic-nro/slidenx-diagnostic.nro`.

## Console test

1. Back up the SD card. Copy `slidenx-diagnostic.nro` to `sd:/switch/slidenx-diagnostic.nro`.
2. Launch the NRO through Homebrew Menu. If GPIO service access is denied in applet mode, retry using title takeover/full-memory homebrew mode.
3. Confirm that the screen reports SD, audio and GPIO status. Press A and B to test sound output. Physically attach/detach each Joy-Con and record the raw pin values and transitions.
4. Exit with PLUS.

## Important interpretation

This NRO is useful as a safe first smoke test, but it does **not** validate the sysmodule's NPDM service permissions, automatic startup, background lifetime, MP3 decoding, or boot flags. If GPIO open fails only in the NRO, that can be a process-permission limitation and does not by itself prove the sysmodule will fail. If the raw values change here, that is useful evidence for the inherited pad mapping but still requires a later sysmodule test.

Do not install the sysmodule until a separate NSP build has succeeded and the diagnostic output has been reviewed. A compiled NRO is not by itself confirmation of hardware compatibility.
