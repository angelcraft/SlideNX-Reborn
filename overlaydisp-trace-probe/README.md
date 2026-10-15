# OverlayDisp Reference Scan v9

This NRO is compiled by GitHub Actions; local devkitPro is not required.

## Why this build exists

The earlier trace captured inconsistent register values. Static inspection of the original instructions shows the two candidate callsites should pass the same string pointer, while the older report sometimes attributed a different string to them. Do not make a sound patch from those inconsistent register snapshots.

Version 9 therefore performs a **read-only cross-reference scan** around module offsets 0x310000–0x320000 to look for AArch64 ADRP instructions that reference the pages containing the previously observed SeJointOut and SeBatteryRateOut strings. When possible, it verifies the following ADD instruction resolves to the exact string pointer.

It does **not** set software breakpoints or single-step. It does attach to overlayDisp to read memory, so the HOME interface may pause briefly during the scan. Run this only on the HOME screen, never during a game.

## Enable the Atmosphère debugger

In sd:/atmosphere/config/system_settings.ini, under the existing [atmosphere] section, merge (do not replace the whole file) these settings:

    enable_standalone_gdbstub = u8!0x1
    enable_htc = u8!0x0

Reboot the Switch. Keep it on your private local Wi-Fi; do not expose or port-forward TCP port 22225 to the Internet.

## Run the scan

1. Download the OverlayDispTraceProbe-NRO artifact from the successful GitHub Actions run for this branch.
2. Replace sd:/switch/overlaydisp-trace-probe.nro with the NRO inside the ZIP. It identifies itself as OverlayDispReferenceScan v9.
3. Launch it from Homebrew Menu and press A once.
4. Wait for the scan to finish and the status to say v9 xref scan complete.
5. Retrieve sd:/switch/OverlayDispTrace/report.txt and send it back.

No Joy-Con disconnect/reconnect sequence is needed for this build. It does not write an IPS/exefs patch or persistently modify files on the SD. It reads a small code range while attached, detaches, and saves the report.

If the console stays paused after the scan, wait briefly for the status first. If HOME remains frozen, hold POWER to shut down fully and reboot; no software breakpoints were installed by v9.
