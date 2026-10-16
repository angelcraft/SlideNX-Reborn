# OverlayDisp JointOut Runtime Probe

This is a read-only runtime diagnostic NRO for the specific executable with Title ID 010000000000100C and build ID prefix 69f8ce5b22dd143a78b4e0a95863c200c0de703a.

It is not an IPS patch. It temporarily inserts debugger breakpoints and a write-watchpoint in RAM, attempts to remove them on exit, and writes a report to:

`sdmc:/switch/OverlayDispJointOutProbe/report.txt`

It does not modify joint_out.raw, the executable on disk, any installed IPS file, or overlayDisp_action.bksnd.

## Before running

1. Use the same emuMMC/HOME Menu environment in which the previous OverlayDispTraceProbe worked.
2. In `sd:/atmosphere/config/system_settings.ini`, merge these keys under the existing `[atmosphere]` section if needed:

   ```ini
   enable_standalone_gdbstub = u8!0x1
   enable_htc = u8!0x0
   ```

3. Fully reboot after changing configuration. Keep the console on your private local network; do not expose or forward port 22225 to the Internet.
4. Run from the HOME menu, not in a game. Close other debugger tools first. This NRO needs the Atmosphère standalone GDB stub and briefly pauses overlayDisp during some breakpoint hits.

## Test

1. Copy `jointout-runtime-probe.nro` to `sd:/switch/jointout-runtime-probe.nro`.
2. Launch it from Homebrew Menu.
3. The probe attaches to overlayDisp and waits for one Joy-Con disconnect event.
4. It attempts to record the actual byte size supplied to the sound player for SeJointOut, the sample count/cursor in that voice slot, and whether the voice was stopped before reaching its loaded sample count.
5. Press PLUS to abort/exit, or wait for the captured state to finish. Read `sd:/switch/OverlayDispJointOutProbe/report.txt`.

## Interpreting the report

- `JOINT_OUT_RESOURCE ... byte_size=` is the resource-wrapper byte size actually read by overlayDisp at playback setup. It is more useful than SD file size alone.
- `VOICE_SLOT ... samples=` is the sample count stored for that voice slot.
- A cursor at or beyond the stored sample count when the voice reaches the end handler is consistent with normal end-of-resource playback.
- A slot becoming idle while its cursor is still below the stored sample count, before the end handler is reached, is evidence of an early state change/reinitialisation rather than simply a short RAW.
- If the GDB stub rejects the watchpoint, the byte-size capture may still be useful, but the early-stop classification will be incomplete.

This is diagnostic evidence only, not a hardware fix. Do not install a new IPS on the basis of this report without reviewing it first.

## Cleanup

No persistent patch is installed by this NRO. Exit with PLUS. If the console freezes, hold POWER for a full shutdown and reboot; the breakpoints/watchpoint are temporary RAM state. Remove the NRO when finished.
