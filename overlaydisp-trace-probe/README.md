# OverlayDispTraceProbe

A read-only on-console probe for Atmosphere's standalone GDB stub. GitHub Actions builds it; no local devkitPro installation is needed.

## What it does
- Connects to the standalone GDB stub on TCP port 22225 using loopback, then the Switch's local IPv4 address.
- Requests the supported RSP features and process XML.
- Finds overlayDisp (Program ID 010000000000100C) if the process list exposes it.
- Briefly attaches to that process, requests Atmosphere's "monitor get info" output (process, memory map and module information), and detaches.
- Saves the report to sdmc:/switch/OverlayDispTrace/report.txt.
- It does not install an IPS, set breakpoints, write process memory or change executable code.

## Enable the server
In sd:/atmosphere/config/system_settings.ini, merge (do not replace the whole file) these values under the existing [atmosphere] section:

    enable_standalone_gdbstub = u8!0x1
    enable_htc = u8!0x0

Keep the Switch on private local Wi-Fi and reboot. Do not expose or port-forward TCP 22225 to the Internet. Attaching briefly pauses the target process.

## Run it
1. Download OverlayDispTraceProbe-NRO from this workflow's GitHub Actions artifact.
2. Copy overlaydisp-trace-probe.nro to sd:/switch/overlaydisp-trace-probe.nro.
3. Launch it in Homebrew Menu and press A once.
4. Wait for completion, exit with PLUS, then retrieve sd:/switch/OverlayDispTrace/report.txt.
5. Send the report back for analysis.

## Limits
This first stage collects a process/module snapshot; it does not yet trace audio events. Only after the report confirms the running module mapping should a follow-up build add breakpoints at verified offsets. A successful cloud build does not prove the GDB stub is enabled or reachable at runtime.
