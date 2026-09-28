# Phawx ON feature matrix

This table lists what Phawx ON 1.1 supports.

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
| QAM-style slide-out overlay | Yes | `src/ui.c` | Topmost panel that never takes focus. Slides in from the right edge of the foreground monitor. DPI-aware. Tabs: Quick / CPU / GPU / Display / System / Plugins / Settings. System is split into sections: Power (the former Power tab), Fan control, Lighting, Windows, one per plugin, Hardware info. |
| "Phawx ON" / "Phawx OFF" header | Yes | `src/ui.c` (header draw), `src/autotdp.c` (`auto.on`) | Green "Phawx ON" while AutoTDP runs, "Phawx OFF" otherwise. Below it, a live strip shows FPS, target and clocks. |
| Overlay only on request | Yes | `src/ui.c`, `src/main.c` | Opens only from the tray, the button combo, the edge swipe or Ctrl+Alt+P. Starts hidden with `/tray`. |
| Intel TDP (PL1/PL2) | Yes | `src/cpu/intel.c` (`cpu.tdp`, `cpu.tdpboost`) | MSR 0x610 with the real 0x606 units. Writes the whole field: enable, clamp and time window are kept. PL2 is raised to PL1 if it would end up lower. Controls hide if the BIOS locked the MSR. |
| Intel PL1/PL2 time windows | Yes | `src/cpu/intel.c` (`cpu.pl1time`, `cpu.pl2time`) | Advanced. |
| AMD TDP (STAPM / fast / slow / APU slow) | Yes | `src/cpu/amd.c`, `src/cpu/smu.c` | SMU mailbox through the PawnIO RyzenSMU module. Covers Raven through Strix Halo / Dragon Range / Fire Range. Keeps a separate boost (fast) limit that is never below TDP. Van Gogh, Mendocino, Dragon Range and Fire Range have no readable power table: there, Boost at Default follows TDP (as the slow limit does), and the limits are not restored on exit because their original values are unknown. |
| AMD temperature limit | Yes | `src/cpu/amd.c` (`cpu.tctl`) | |
| AMD STAPM / slow time constants | Yes | `src/cpu/amd.c` (`cpu.stapmtime`, `cpu.slowtime`) | Advanced. |
| EPP | Yes | `src/sys/winpower.c` (`power.epp`, `cpu.epp_p`, `cpu.epp_e`) | Windows PERFEPP / PERFEPP1. Hybrid CPUs get a separate value per core class. Restored on exit. |
| Hybrid core control | Yes | `src/sys/winpower.c` | Per-class core parking (CPMIN/CPMAXCORES and the class-1 versions), per-class frequency caps, long and short thread scheduling policy, hetero policy and thresholds (advanced). |
| Fewest-cores search under AutoTDP | Yes | `src/autotdp.c` (`auto.cores`) | On by default, on CPUs with 3+ cores. Parks cores through CPMAXCORES (per class on hybrid CPUs) one trial at a time and keeps a step only if the power reading drops at the same FPS. Floors: 2 cores; on hybrid 2 P + 1 E. Reads the real parked state with `GetSystemCpuSetInformation` to skip limits that change nothing and to learn how Windows rounds. Restored without a game, on stop and after a crash. |
| CPU clock control, AMD | Yes | `src/cpu/amd.c` | Per-core CPPC max perf (0xC00102B3). This is the fast CPU clock domain AutoTDP uses. Core Performance Boost toggle (`cpu.cpb`). |
| CPU clock control, Intel | Partial | `src/sys/winpower.c` (`cpu.freqcap`, `cpu.freqcap_p`) | Only through the Windows PROCFREQMAX / PROCFREQMAX1 cap. The IntelMSR module does not allow writes to HWP_REQUEST (0x774) or PERF_CTL (0x199), so there is no MSR clock path. |
| GPU clocks, AMD dGPU | Yes | `src/gpu/amd.c` (`amdgpu.maxclk`, `amdgpu.minclk`, `amdgpu.power`) | ADLX manual graphics tuning, with ADL Overdrive8 as a fallback. |
| GPU clocks, AMD iGPU | Yes | `src/cpu/amd.c` (`gpu.igpumin`, `gpu.igpumax`) | SMU gfxclk min/max. Setting min equal to max gives a static clock. |
| GPU clocks, NVIDIA | Yes | `src/gpu/nvidia.c` (`nvidia.maxclk`, `nvidia.minclk`, `nvidia.power`) | NVML locked clocks and power limit. Reset on exit. |
| GPU clocks, Intel iGPU / Arc | Yes | `src/gpu/intel.c` (`intelgpu.*`, `arc.*`) | Intel IGCL (ControlLib) frequency range and power limits. |
| AutoTDP: FPS target at the least power | Yes | `src/autotdp.c` | Raises clocks fast when FPS drops and lowers them slowly once FPS is stable; near the limit a clock step is kept unless the power reading rises. Picks CPU or GPU as the bottleneck. Then tries fewer cores and a higher EPP, one at a time, kept only when the machine draws clearly less with the frame rate and 1% lows held (the clocks may rise to make up for it). "EPP while active" (`auto.epp0`): Windows / 0 (fastest) / Tune for power (default). When the target is out of reach it holds what it reaches with the least power. Target can be the refresh rate, half the refresh rate, or a custom value. You can set floor and ceiling limits. |
| Power meter | Yes | `src/power.c` | Battery discharge rate (IOCTL_BATTERY_QUERY_STATUS, system batteries that report mW, not UPSes, at most every 2 s) for the whole machine on battery, but not while a charger is connected (a weak one leaves the battery covering only the shortfall); otherwise CPU package (Intel RAPL, AMD PM table) plus NVIDIA GPU power, counted only when every reader answered. AutoTDP reads it only while a game runs. Shown as "Power use" on the Quick page and "Battery draw" in Hardware info. |
| AutoTDP simulation | Yes | `tests/autotdp_sim.c` (`make sim`) | autotdp.c against a simulated handheld with a fake OS clock: GPU- and CPU-bound games, scene change, target out of reach, battery-only and no power reading, game quit, crash. Checks FPS held, power against the best setting found by brute force, core floors, settling, and restore. |
| FPS source | Yes | `src/fps.c` | Intel PresentMon service (PresentMonAPI2). The DLL is only loaded from Program Files. There is no in-process ETW fallback. |
| Per-game profiles | Partial | `src/main.c` (`app.profiles`), `src/config.c` | Off by default. "Save profile for current game" (Settings) turns them on. Switches automatically on the foreground exe, using `[game:<exe>]` sections. Controls flagged `CF_PROFILE` are saved. The AutoTDP on/off state itself is global, not per game. Configs from 1.0 that already have profiles keep them on. |
| Gamepad navigation | Partial | `src/input.c` | XInput pads, including the Guide button through XInputGetStateEx. Y pins or unpins the selected setting; holding X resets it. Non-XInput HID pads (for example DualSense, or some vendor handheld buttons) are not read. |
| Button combo to open | Yes | `src/input.c`, `src/main.c` (`app.combo`) | Default is Guide, or Back+Start. Settings → "Open menu combo" records any combo of two or more held buttons. |
| Touch | Yes | `src/input.c`, `src/ui.c` | Swipe left from the top-right screen edge to open. Tap, drag and scroll inside the panel. Tap a row's pin to pin it (on a slider, on its label line; the track line always drags). Press and hold a saved setting for 2 seconds to reset it (0.5 s with a mouse). Moving more than about 10 px cancels a tap or hold, and up/down movement scrolls. Windows' press-and-hold ring, pen tap and barrel feedback and pen flicks are turned off for the panel. |
| Tray icon | Yes | `src/main.c` | Click toggles the menu. The right-click menu has Open, AutoTDP, Start with Windows and Exit. |
| Keyboard | Partial | `src/main.c`, `src/ui.c`, `src/input.c` | The Ctrl+Alt+P hotkey works. Holding R resets the selected setting; it is read with raw input only while the menu is open, so the key also reaches the game. Other in-panel keys need focus, which the no-activate panel never gets. |
| Start with Windows | Yes | `src/main.c` (`app.autostart`) | ONLOGON scheduled task with highest privileges. Refused unless the exe is under Program Files. |
| Restore on exit, crash and resume | Yes | `src/main.c`, `src/registry.c`, each backend's `shutdown`/`resume` | Windows power values, CPPC, SMU limits, the Intel PL and NVML clocks are restored on exit and from the crash handler, built-ins first, then plugins, then the PawnIO driver is closed. A crash in a plugin no longer stops the rest from being restored. Settings are reapplied after resume, on AC/DC change and on profile switches. 1.1 fixes the crash handler, which never ran in 1.0 because the exe was built without unwind tables. |
| Safe DLL loading | Yes | `src/main.c`, `src/util.c`, `src/plugins.c` | The elevated process never loads DLLs from `PATH` or the current directory (`SetDefaultDllDirectories`), plugins and the libraries they use included. Vendor DLLs load from System32 or by full path; PresentMon and the NVML fallback only from the real Program Files folder, not `%ProgramFiles%`. Plugins load only when Phawx ON is under Program Files. |
| Extensible backend design | Yes | `src/phawx.h`, `src/registry.c` | `ph_backend` with probe/init/shutdown/resume/tick, generic `ph_ctl` controls rendered by the UI, and `ph_clk` clock domains that AutoTDP drives. See the README. |
| Pins on the Quick page | Yes | `src/pins.c`, `src/ui.c` | Every setting and action has a pin, except on the Quick page itself and the Plugins page's own rows. Tap it or press Y to add or remove the setting. Pinned settings show in a Pinned section below the built-in Quick controls, each with the page it came from. Up to 64, saved in `[pins]`. |
| Hold to reset | Yes | `src/ui.c` (`hold_*`), `src/input.c` | Hold X (gamepad) or R (keyboard) for 0.5 s, or touch and hold for 2 s (mouse 0.5 s), to set the selected setting back to its default. A bar fills along the row while you hold. Only saved settings reset: on a setting already at Default it says "Already at Default", on rows locked by AutoTDP "Managed by AutoTDP", and on actions, links and plugin switches a long press is an ordinary tap. |
| Plugins page | Yes | `src/plugins.c` | Status of the two required components (PawnIO and PresentMon, with version: "… is loaded and running", or what is wrong, such as "PawnIO modules are missing" or "PresentMon service is not running") and a Download link for each one that is missing. Below that, every plugin found, with an on/off switch and its status ("Running", "Not running · <reason>"). |
| DLL plugins | Yes | `src/plugins.c`, `plugins/sdk/phawx_plugin.h`, `docs/PLUGINS.md` | C-ABI DLLs in `plugins\` next to the exe (or one folder below) that export `phx_plugin_init`. They can add controls, AutoTDP clock domains, fans and RGB lights, and they get config, logging and PawnIO EC/MSR access. Off by default, loaded only from Program Files, turning one on asks first and applies on restart. A plugin that crashes or hangs Phawx ON at start, or crashes it later (in its own DLL, a DLL from its folder, or while Phawx ON is calling it), is never called again and is turned off on the next start; everything else is still restored. At most 16 plugins, 48 controls and 4 clocks, fans and lights each. The guide ships in the zip as `sdk\PLUGINS.md`. |
| Fans and RGB lights from plugins | Yes | `src/plugins.c` (`h_add_fan`, `h_add_rgb`) | The host builds the rows in the System page's Fan control and Lighting sections (mode, speed, curve, RPM; mode, colour, brightness, effect) and hands the hardware back to firmware as soon as the mode returns to Default, on exit, and after a crash elsewhere (not after the plugin itself crashed). |
| Fan curves | Yes | `src/fans.c`, `src/ui.c` (`draw_curve`) | "Curve" fan mode for the GPD fan and every plugin fan. 7 points (30–90 °C) edited on a graph by touch, mouse or gamepad; the duty follows the CPU temperature (Intel package, AMD Tctl from the PM table or the SMN thermal register) once a second, up at once and down by at most 5 %/s, 100 % at 95 °C, back to the firmware without a temperature for 5 s. Saved as `<fan>.curve`, not per game. |
| Hardware info table | Yes | `src/sys/hwinfo.c` | Last section of the System page: manufacturer, model, board, BIOS, Windows; CPU name, cores, core types, load, clock and the CPU backend's power and temperature; memory in use; every GPU, display and fan reading; power source, battery and its discharge rate; PawnIO, PresentMon and the frame rate. Info rows of plugins on the CPU, GPU, Display and System pages are included. |
| RyzenAdj plugin | Partial | `plugins/ryzenadj/` | Off by default. Uses `libryzenadj.dll` and WinRing0 from `plugins\ryzenadj\`: while it runs, any program on the PC can use WinRing0 to take full control of it. Adds STAPM, fast and slow limits, the temperature limit and (Raven to Lucienne) an iGPU clock domain for AutoTDP. Stands down when the built-in AMD backend already controls the APU. Limits are read back and restored on exit only with the opt-in "Read power table", which installs the InpOut driver as a service that stays after exit (and is not offered on Van Gogh, Mendocino, Dragon Range or Fire Range). Every libryzenadj call is time-limited. |

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
| Min/max processor state, autonomous mode and window, duty cycling | Yes | `src/sys/winpower.c` (System page, Power section; the tuning rows are advanced) |
| Radeon Chill, FRTC, Anti-Lag, Boost, RSR, RIS / sharpness | Yes | `src/gpu/amd.c` (`radeon.*`), plus an optional driver restart action |
| Radeon AFMF, Vari-Bright, VSR, colour depth, UMD settings | No | Could be added to the registry table in `src/gpu/amd.c`. |
| EDID overrides, DAL restricted modes, integer scaling | Not planned | Destructive display registry edits. |
| Refresh rate / resolution (qres) | Yes | `src/sys/display.c` (`display.refresh`, `display.res`) |
| GPD Win Mini fan (EC 0x7A) | Yes | `src/sys/devices.c` (`dev.fanmode`, `dev.fanspeed`, curve `dev.fancurve`) through LpcACPIEC. Auto, Manual, Full speed or Curve. Picking Default hands the fan back to the firmware at once. The ITE 0x4E/0x4F path cannot work with LpcACPIEC. Only the ACPI EC fallback runs. |
| Modern Standby override | Yes | `src/sys/tweaks.c` (`sys.nomodernstandby`, danger) |
| Game Mode, Game Bar capture, HAGS, touch keyboard | Yes | `src/sys/tweaks.c` (new) |
| SysMain disable | No | |
| NVIDIA PCIe link speed, "error 43" fix, HDCP toggle | No | |
| Dock / undock and battery TDP override | Partial | Settings are reapplied on AC/DC change, but there are no separate AC and DC values yet. |
| TDP / GPU up and down hotkeys | No | Use the overlay. Dedicated step hotkeys could be added in `src/main.c`. |
| Hardware info screen | Partial | The System page's Hardware info table shows everything Phawx ON reads (see above). Not read yet: per-core clocks and temperatures, motherboard sensor chips, RAM modules, drive health. |
