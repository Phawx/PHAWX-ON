# Phawx ON feature matrix

This table lists what Phawx ON 1.0 supports.

**Status**
- **Yes**: implemented.
- **Partial**: implemented with the limits noted.
- **No**: not implemented yet. A backend could add it.
- **Blocked**: the signed PawnIO modules do not allow this access, so it cannot be done without an unsigned driver.
- **Not planned**: left out on purpose (safety, security, or no benefit).

Control keys (such as `cpu.tdp`) are the names used in `phawx.ini`.

## Core requirements

| Feature | Status | Where | Notes |
|---|---|---|---|
| QAM-style slide-out overlay | Yes | `src/ui.c` | Topmost panel that never takes focus. Slides in from the right edge of the foreground monitor. DPI-aware. Tabs: Quick / CPU / Power / GPU / Display / System / Settings. |
| "Phawx ON" / "Phawx OFF" header | Yes | `src/ui.c` (header draw), `src/autotdp.c` (`auto.on`) | Green "Phawx ON" while AutoTDP runs, "Phawx OFF" otherwise. Below it, a live strip shows FPS, target and clocks. |
| Overlay only on request | Yes | `src/ui.c`, `src/main.c` | Opens only from the tray, the button combo, the edge swipe or Ctrl+Alt+P. Starts hidden with `/tray`. |
| Intel TDP (PL1/PL2) | Yes | `src/cpu/intel.c` (`cpu.tdp`, `cpu.tdpboost`) | MSR 0x610 with the real 0x606 units. Writes the whole field: enable, clamp and time window are kept. PL2 is raised to PL1 if it would end up lower. Controls hide if the BIOS locked the MSR. |
| Intel PL1/PL2 time windows | Yes | `src/cpu/intel.c` (`cpu.pl1time`, `cpu.pl2time`) | Advanced. |
| AMD TDP (STAPM / fast / slow / APU slow) | Yes | `src/cpu/amd.c`, `src/cpu/smu.c` | SMU mailbox through the PawnIO RyzenSMU module. Covers Raven through Strix Halo / Dragon Range / Fire Range. Keeps a separate boost (fast) limit. |
| AMD temperature limit | Yes | `src/cpu/amd.c` (`cpu.tctl`) | |
| AMD STAPM / slow time constants | Yes | `src/cpu/amd.c` (`cpu.stapmtime`, `cpu.slowtime`) | Advanced. |
| EPP | Yes | `src/sys/winpower.c` (`power.epp`, `cpu.epp_p`, `cpu.epp_e`) | Windows PERFEPP / PERFEPP1. Hybrid CPUs get a separate value per core class. Restored on exit. |
| Hybrid core control | Yes | `src/sys/winpower.c` | Per-class core parking (CPMIN/CPMAXCORES and the class-1 versions), per-class frequency caps, long and short thread scheduling policy, hetero policy and thresholds (advanced). |
| Dynamic P/E parking under AutoTDP | Yes | `src/autotdp.c` (`auto.cores`) | Advanced. Only on hybrid CPUs. |
| CPU clock control, AMD | Yes | `src/cpu/amd.c` | Per-core CPPC max perf (0xC00102B3). This is the fast CPU clock domain AutoTDP uses. Core Performance Boost toggle (`cpu.cpb`). |
| CPU clock control, Intel | Partial | `src/sys/winpower.c` (`cpu.freqcap`, `cpu.freqcap_p`) | Only through the Windows PROCFREQMAX / PROCFREQMAX1 cap. The IntelMSR module does not allow writes to HWP_REQUEST (0x774) or PERF_CTL (0x199), so there is no MSR clock path. |
| GPU clocks, AMD dGPU | Yes | `src/gpu/amd.c` (`amdgpu.maxclk`, `amdgpu.minclk`, `amdgpu.power`) | ADLX manual graphics tuning, with ADL Overdrive8 as a fallback. |
| GPU clocks, AMD iGPU | Yes | `src/cpu/amd.c` (`gpu.igpumin`, `gpu.igpumax`) | SMU gfxclk min/max. Setting min equal to max gives a static clock. |
| GPU clocks, NVIDIA | Yes | `src/gpu/nvidia.c` (`nvidia.maxclk`, `nvidia.minclk`, `nvidia.power`) | NVML locked clocks and power limit. Reset on exit. |
| GPU clocks, Intel iGPU / Arc | Yes | `src/gpu/intel.c` (`intelgpu.*`, `arc.*`) | Intel IGCL (ControlLib) frequency range and power limits. |
| AutoTDP: FPS target at the lowest clocks | Yes | `src/autotdp.c` | Sets EPP to 0 while running (restored on stop). Raises clocks fast when FPS drops and lowers them slowly once FPS is stable. Picks CPU or GPU as the bottleneck. Target can be the refresh rate, half the refresh rate, or a custom value. You can set floor and ceiling limits. |
| FPS source | Yes | `src/fps.c` | Intel PresentMon service (PresentMonAPI2). The DLL is only loaded from Program Files. There is no in-process ETW fallback. |
| Per-game profiles | Partial | `src/main.c`, `src/config.c` | Switches automatically on the foreground exe, using `[game:<exe>]` sections. Controls flagged `CF_PROFILE` are saved. The AutoTDP on/off state itself is global, not per game. |
| Gamepad navigation | Partial | `src/input.c` | XInput pads, including the Guide button through XInputGetStateEx. Non-XInput HID pads (for example DualSense, or some vendor handheld buttons) are not read. |
| Button combo to open | Yes | `src/input.c`, `src/main.c` (`app.combo`) | Default is Guide, or Back+Start. Settings → "Open menu combo" records any combo of two or more held buttons. |
| Touch | Yes | `src/input.c`, `src/ui.c` | Swipe left from the top-right screen edge to open. Tap, drag, scroll and long-press to reset inside the panel. |
| Tray icon | Yes | `src/main.c` | Click toggles the menu. The right-click menu has Open, AutoTDP, Start with Windows and Exit. |
| Keyboard | Partial | `src/main.c`, `src/ui.c` | The Ctrl+Alt+P hotkey works. In-panel key handling exists, but the panel is no-activate and never gets keyboard focus. |
| Start with Windows | Yes | `src/main.c` (`app.autostart`) | ONLOGON scheduled task with highest privileges. Refused unless the exe is under Program Files. |
| Restore on exit, crash and resume | Yes | `src/main.c`, each backend's `shutdown`/`resume` | Windows power values, CPPC, SMU limits, the Intel PL and NVML clocks are restored on exit and from the crash handler. Settings are reapplied after resume and on AC/DC change. |
| Extensible plugin design | Yes | `src/phawx.h`, `src/registry.c` | `ph_backend` with probe/init/shutdown/resume/tick, generic `ph_ctl` controls rendered by the UI, and `ph_clk` clock domains that AutoTDP drives. See the README. |

## Additional capabilities

| Feature | Status | Where / reason |
|---|---|---|
| Intel MCHBAR MMIO power limit mirror (0x59A0/0x59A4) | Blocked | The IntelMCHBAR module is read-only. If firmware set a lower MMIO limit, it still wins over the MSR value. |
| Intel power limit lock (0x610 bit 63) | Not planned | It sticks until reboot and blocks later changes. |
| Intel turbo ratio limits (0x1AD) | Blocked | Not in the IntelMSR write allowlist. |
| Intel undervolt (0x150 planes) | Not planned | Undervolting is excluded by design. |
| Intel HWP request / EPP MSR (0x774), EPB (0x1B0) | Blocked | Not in the write allowlist. EPP goes through Windows PERFEPP instead. |
| Intel PP0/PP1 priority (0x63A/0x642) | Blocked | Not in the write allowlist. |
| Intel uncore ratio (0x620) | Blocked | Not in the write allowlist. |
| Intel race-to-halt (0x1FC) | Blocked | Not in the write allowlist. |
| Intel iGPU GT limit (MCHBAR+0x5994) | Blocked / replaced | Intel iGPU frequency is set through IGCL instead (`src/gpu/intel.c`). |
| Intel Graphics power plan (powercfg 44f3beca) | No | Could be added to `src/sys/winpower.c`. |
| Intel DPTF device disable | Not planned | Removes a thermal safety layer. |
| iGPU VRAM (DedicatedSegmentSize) | Not planned | Registry change that needs a driver restart. |
| AMD `--max-performance` / `--power-saving` | No | Could be added to `src/cpu/smu.c` / `src/cpu/amd.c`. |
| AMD core CC6 (0xC0010296) | No | AMDFamily17 allows writes to this MSR. Could be added to `src/cpu/amd.c`. |
| Steam Deck GFX VRM current | No | Van Gogh only. Could be added as an SMU message. |
| Windows boost mode (PERFBOOSTMODE) | Yes | `src/sys/winpower.c` (`power.boost`) |
| PROCFREQMAX / PROCFREQMAX1 caps | Yes | `src/sys/winpower.c` |
| Core parking CPMIN/CPMAX | Yes | `src/sys/winpower.c`. |
| SCHEDPOLICY / SHORTSCHEDPOLICY | Yes | `src/sys/winpower.c` (`cpu.sched`, `cpu.shortsched`) |
| Power mode slider (overlay) | Yes | `src/sys/winpower.c` (`power.mode`) |
| Min/max processor state, autonomous mode and window, duty cycling | Yes | `src/sys/winpower.c` (Power page, advanced) |
| Radeon Chill, FRTC, Anti-Lag, Boost, RSR, RIS / sharpness | Yes | `src/gpu/amd.c` (`radeon.*`), plus an optional driver restart action |
| Radeon AFMF, Vari-Bright, VSR, colour depth, UMD settings | No | Could be added to the registry table in `src/gpu/amd.c`. |
| EDID overrides, DAL restricted modes, integer scaling | Not planned | Destructive display registry edits. |
| Refresh rate / resolution (qres) | Yes | `src/sys/display.c` (`display.refresh`, `display.res`) |
| GPD Win Mini fan (EC 0x7A) | Yes | `src/sys/devices.c` (`dev.fanmode`, `dev.fanspeed`) through LpcACPIEC. The ITE 0x4E/0x4F path cannot work with LpcACPIEC. Only the ACPI EC fallback runs. |
| Modern Standby override | Yes | `src/sys/tweaks.c` (`sys.nomodernstandby`, danger) |
| Game Mode, Game Bar capture, HAGS, touch keyboard | Yes | `src/sys/tweaks.c` (new) |
| SysMain disable | No | |
| NVIDIA PCIe link speed, "error 43" fix, HDCP toggle | No | |
| Dock / undock and battery TDP override | Partial | Settings are reapplied on AC/DC change, but there are no separate AC and DC values yet. |
| TDP / GPU up and down hotkeys | No | Use the overlay. Dedicated step hotkeys could be added in `src/main.c`. |
| Hardware info screen | Partial | CPU page shows package power and temperature. GPU page shows clock, load, power and temperature. Settings shows the CPU and driver. |
