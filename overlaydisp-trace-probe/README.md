# OverlayDispTraceProbe — SeJointOut runtime trace

This NRO is compiled by GitHub Actions; local devkitPro is not required.

## What it measures
The report from the first probe located `overlayDisp` (Program ID `010000000000100C`) and revealed its runtime module base. The current trace build uses that base to set two temporary software breakpoints at verified AArch64 call sites for the `SeJointOut` cue (module offsets `0x315040` and `0x31507c`).

The NRO:
- Connects to Atmosphère's standalone GDB stub.
- Finds the PID for `overlayDisp`, attaches, and decodes `monitor get info`.
- Temporarily sets two software breakpoints in the running process and counts hits at each route.
- Removes the breakpoints and detaches when stopped with PLUS.
- Saves results to `sdmc:/switch/OverlayDispTrace/report.txt`.

It does not write an IPS/exefs patch or persistently modify files on the SD card. The debugger does temporarily patch instructions in RAM while tracing and briefly pauses `overlayDisp` at each hit. Do not use it while playing a game; run it from Homebrew Menu on the HOME screen.

## Enable the Atmosphère debugger
In `sd:/atmosphere/config/system_settings.ini`, under the existing `[atmosphere]` section, merge (do not replace the whole file) these settings:

    enable_standalone_gdbstub = u8!0x1
    enable_htc = u8!0x0

Reboot the Switch. Keep it on your private local Wi-Fi; do not expose or port-forward TCP port 22225 to the Internet.

## Run the trace
1. Download the `OverlayDispTraceProbe-NRO` artifact from this workflow's successful GitHub Actions run.
2. Replace the old `sd:/switch/overlaydisp-trace-probe.nro` with the new one.
3. Launch it from Homebrew Menu and press A once.
4. Wait until the screen displays the trace counters.
5. Disconnect and reconnect a Joy-Con a few times to reproduce the issue.
6. Press PLUS to stop. Wait for the final status, exit normally, and retrieve `sd:/switch/OverlayDispTrace/report.txt`.
7. Send that report back for analysis.

If no route hits are observed, that is useful evidence too. If the screen reports an unexpected stop or an inability to set/remove breakpoints, send that report and do not run it again until reviewed. If Atmosphère or HOME freezes, hold POWER to shut down fully and reboot; the runtime breakpoints are not persistent.

## Interpretation
A hit at either call site shows that path requested `SeJointOut`; it does not by itself prove that the sound played to completion. If both sites are hit repeatedly for one physical disconnect, that supports a retrigger/duplicate-event hypothesis. If only one is hit and the audio still cuts, the next investigation must instrument the downstream audio-voice submission/stop path.
