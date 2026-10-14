# GitHub Actions build

This repository now includes `.github/workflows/build.yml` on `main`.

The workflow triggers on pushes to `main`/`master`, pull requests targeting those branches, and manual `workflow_dispatch`. It installs devkitPro's Switch toolchain and `switch-mpg123`, runs `build_and_package.sh`, validates the staged SD-card layout, and uploads `SlideNX-Reborn-SD-package` as a workflow artifact when successful. On failure it attempts to upload compiler diagnostics.

## Run it manually

1. Open the repository's **Actions** tab: https://github.com/angelcraft/SlideNX-Reborn/actions
2. Select **SlideNX Reborn build** in the workflow list.
3. Click **Run workflow** and select `main`.
4. Open the run to watch build output. A successful run exposes `SlideNX-Reborn-SD-package` under **Summary → Artifacts**.
5. If it fails, inspect the failed step logs and download `SlideNX-Reborn-build-diagnostics` if available.

## What a green build means

A successful CI run proves only that the selected toolchain compiled and packaged the source. It does not establish that raw GPIO identifiers `0x0C` and `0x34` work on every Switch OLED or Horizon 22.x point release. Test with an SD backup and preferably emuMMC first. The original behavior of not suppressing Nintendo's own connection sound remains a limitation.

## Install/package layout

The artifact contains a `dist/` directory's contents staged for the SD root, including `atmosphere/contents/420000000000534E/exefs.nsp`, its `flags/boot2.flag`, and `SlideNX/config.ini`. Add MP3 files under `SlideNX/sounds/` yourself.
