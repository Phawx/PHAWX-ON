# Phawx ON

Phawx ON is a small, open-source performance tuning app for Windows handhelds and laptops. It is a native C tray app of about 250 KB with no runtime dependencies. It adds a Steam Deck–style quick-access overlay where you set TDP, EPP, CPU and GPU clocks, and hybrid P/E core behaviour. It also has an **AutoTDP** mode that holds a target frame rate at the lowest CPU and GPU clocks that still reach it.

It is built to be a good citizen:

- Hardware access goes only through the **signed PawnIO driver and its signed modules**.
- Frame data comes only from **Intel PresentMon**. It does not inject into or hook games.
- The overlay appears only when you ask for it.
- It never turns off security features: no HVCI, driver blocklist, VBS or Defender changes.
- No undervolting.
- Changes are reverted when the app exits or crashes, and reapplied after sleep.

See [docs/FEATURES.md](docs/FEATURES.md) for the full feature matrix, including what is not supported and why.

> Hardware behaviour cannot be tested in CI. Every hardware control starts at **Default**, which means Phawx ON leaves that setting alone until you change it.

## Features

- **Overlay.** Slides in from the right edge, and gamepad, touch and mouse all work in it. The header reads **Phawx ON** (green) while AutoTDP runs and **Phawx OFF** otherwise, with live FPS and clocks below it. Pages: Quick, CPU, Power, GPU, Display, System, Settings.
- **TDP.**
  - Intel: PL1/PL2 and their time windows, through MSR 0x610 with the real power units.
  - AMD Ryzen APUs (Raven through Strix Halo): sustained, boost, slow and APU slow limits, time constants and temperature limit, through the SMU.
- **EPP.** Windows energy performance preference, set separately for P-cores and E-cores on hybrid CPUs.
- **Hybrid cores.** Per-class core parking, per-class clock caps, and thread scheduling policy (prefer P or E cores).
- **CPU clocks.**
  - AMD: per-core CPPC max clock and the Core Performance Boost toggle.
  - Both vendors: the Windows maximum-frequency cap.
- **GPU clocks.**
  - AMD dGPU: ADLX / ADL.
  - AMD iGPU: SMU gfxclk.
  - NVIDIA: NVML locked clocks and power limit.
  - Intel iGPU / Arc: IGCL frequency and power limits.
  - Radeon Chill, frame rate target, Anti-Lag, Boost, RSR and sharpening.
- **AutoTDP.**
  - Sets EPP to 0.
  - Watches the foreground game's FPS through PresentMon.
  - Raises CPU or GPU clocks quickly when FPS drops, and lowers them slowly once FPS is stable.
  - Detects whether the CPU or the GPU is the bottleneck.
  - Can park P/E cores dynamically.
  - Target can be the refresh rate, half the refresh rate, or a custom value.
- **Per-game profiles.** Save the current settings for the game in front. They load automatically whenever that game is in the foreground.
- **Display.** Refresh rate and resolution.
- **System.** Game Mode, Game Bar capture, GPU hardware scheduling, touch keyboard, Modern Standby override, and GPD Win Mini fan control.

## Requirements

- Windows 10 21H2 or later, or Windows 11, x64. The app asks for administrator rights when it starts.
- **PawnIO**, needed for TDP, CPU clock, AMD iGPU clock and fan control:
  1. Install the PawnIO driver from <https://pawnio.eu>. This installs `PawnIOLib.dll` and the driver.
  2. Put the signed modules from the [PawnIO.Modules releases](https://github.com/namazso/PawnIO.Modules/releases) in a `modules` folder. Use the PawnIO install folder (usually `C:\Program Files\PawnIO\modules\`) or `modules\` next to `PhawxON.exe`. Phawx ON loads:
     - Intel: `IntelMSR.bin`, `IntelMCHBAR.bin`
     - AMD: `AMDFamily17.bin`, `RyzenSMU.bin`
     - Fan: `LpcACPIEC.bin`

  The driver only runs code signed by the PawnIO project. Phawx ON can therefore only do what those modules allow. On Intel this means the power limit (0x610) is available, but turbo ratios and HWP MSRs are not. Without PawnIO, the Windows power, GPU vendor API and display controls still work.
- **Intel PresentMon** (only needed for AutoTDP and the FPS readout): install PresentMon 2.x from <https://game.intel.com/story/intel-presentmon/> or [GitHub](https://github.com/GameTechDev/PresentMon/releases). Phawx ON uses the PresentMon service and its `PresentMonAPI2.dll`. It only loads that DLL from under Program Files, and starts the service if it is stopped.
- GPU clock control uses each vendor's own driver component: `amdadlx64.dll` / `atiadlxx.dll` (AMD Adrenalin), `nvml.dll` (NVIDIA driver) or `ControlLib.dll` (Intel graphics driver). Controls for a GPU without its driver component are hidden.

## Install

1. Download `PhawxON-1.0.0.zip` from [Releases](https://github.com/Phawx/PhawxON/releases).
2. Extract it to **`C:\Program Files\PhawxON\`**. "Start with Windows" only works from there, because an elevated autostart task must never point at a folder that normal users can write to.
3. Install PawnIO with its modules, and PresentMon if you want AutoTDP (see above).
4. Run `PhawxON.exe`.
5. Optional: turn on **Settings → Start with Windows**. This creates a logon task that starts Phawx ON hidden in the tray (`/tray`).

Settings are stored in `phawx.ini` next to the exe, or in `%APPDATA%\PhawxON\` if that folder cannot be written. To get a debug log (`phawx.log` in the same folder), set the environment variable `PHAWX_LOG=1`.

To uninstall:
1. Turn off Start with Windows.
2. Exit Phawx ON from the tray. It restores every setting it changed.
3. Delete the folder.

## Using it

### Opening and closing the overlay

| Input | Action |
|---|---|
| Gamepad | Press **Guide** (Xbox button). If Guide isn't available, press **Back + Start**. To use your own combo, go to Settings → *Open menu combo*, then hold the buttons you want. |
| Touch | Swipe left from the top-right edge of the screen. |
| Tray icon | Click to toggle. Right-click for AutoTDP, Start with Windows and Exit. |
| Keyboard | **Ctrl + Alt + P** |

The overlay never takes focus from the game.

### Inside the overlay

| Gamepad | Touch / mouse | Action |
|---|---|---|
| D-pad / left stick up/down | Tap, or drag to scroll | Select a row |
| D-pad left/right | Drag the slider | Change the value |
| A | Tap | Toggle or run the selected item |
| LB / RB | Tap a tab | Switch page |
| X | Long-press | Reset the row to **Default** (Phawx ON stops managing it) |
| Y | - | Turn AutoTDP on or off |
| B | - | Close the menu |

Changes take effect about half a second after you stop adjusting. Settings marked dangerous ask for confirmation first. Advanced options are hidden until you turn on **Settings → Show advanced options**.

### AutoTDP

1. On the **Quick** page, turn on **Phawx ON**.
2. Choose a target: refresh rate, half refresh, or custom FPS.
3. Start a game.

The status line shows current and target FPS, the CPU and GPU clocks, and which one is the bottleneck. While AutoTDP runs, the clock and EPP controls it manages are locked and show "AutoTDP". Floors, ceilings, aggressiveness, settle time and dynamic core parking are under **CPU → AutoTDP tuning** (advanced).

AutoTDP works on clocks, not power limits. Set a TDP ceiling on the Quick page if you also want a hard power cap.

### Per-game profiles

1. With the game in front, open the overlay.
2. Set it up as you like.
3. Go to **Settings → Save profile for current game**.

Phawx ON switches profiles when the game gains focus and switches back to your global settings when it loses focus.

## Build

You need `x86_64-w64-mingw32-gcc` and `windres` from mingw-w64 (Debian/Ubuntu: `apt install mingw-w64`, or MSYS2 on Windows) and GNU make.

```sh
make -j8        # build/PhawxON.exe
make dist       # dist/PhawxON-1.0.0.zip
make clean
```

The build uses `-Wall -Wextra` and should produce no warnings. `.github/workflows/release.yml` builds the zip and publishes the GitHub release when you trigger it manually.

## Adding a hardware backend

A backend is one C file that controls some hardware. The UI draws its controls automatically, profiles save them, and AutoTDP can drive its clocks. Adding one takes three steps:

1. Create `src/<area>/<name>.c`. The Makefile picks it up automatically.
2. Add `extern ph_backend bk_<name>;` and `&bk_<name>,` to the table in `src/registry.c`.
3. Run `make`.

```c
#include "phawx.h"

static int orig_mhz = -1, wrote;

static int get_clk(ph_ctl *c, int32_t *out) { /* read hardware */ *out = 1200; return 0; }
static int set_clk(ph_ctl *c, int32_t mhz)  { /* write hardware */ wrote = 1; return 0; }

static ph_ctl ctls[] = {
    { .key = "mygpu.maxclk", .label = L"Max GPU clock", .type = CT_SLIDER, .page = PG_GPU,
      .flags = CF_OPTIONAL | CF_REAPPLY | CF_PROFILE | CF_AUTOTDP,
      .min = 300, .max = 2000, .step = 50, .unit = L"MHz",
      .get = get_clk, .set = set_clk, .fmt = fmt_mhz, .order = 10 },
};

static int clk_max(ph_clk *d, int mhz) { return set_clk(NULL, mhz); }
static int clk_reset(ph_clk *d)        { return set_clk(NULL, orig_mhz); }

static ph_clk clk = { .name = L"My GPU", .min_mhz = 300, .max_mhz = 2000, .step_mhz = 50,
                      .set_max = clk_max, .reset = clk_reset, .prio = 20 };

static int  probe(void) { return 1; /* does this machine have the hardware? */ }
static int  init(void)  { get_clk(NULL, &orig_mhz); ph_register_ctls(ctls, PH_ARRAY(ctls));
                          ph_register_gpu_clk(&clk); return 0; }
static void shutdown(void) { if (wrote) set_clk(NULL, orig_mhz); }

ph_backend bk_mygpu = { "mygpu", L"My GPU", BK_GPU, probe, init, shutdown, NULL, NULL, 0 };
```

**Rules for backends**
- **`probe`**: return 0 when the hardware isn't present.
- **`init`**:
  - Read the current values so that `def` reflects the hardware.
  - Set `CF_HIDDEN` on anything you can't read.
  - Register your controls and clock domains.
- **`shutdown`**: undo every change you made. It runs on exit, on logoff and from the crash handler.
- **`resume`** (optional): runs after sleep. Controls flagged `CF_REAPPLY` are also reapplied automatically.
- **`tick`** (optional): runs about once a second while the menu is open. Keep it cheap.
- **Hardware access**: go only through `drv_*` (PawnIO) or an official vendor API. Load vendor DLLs with `LOAD_LIBRARY_SEARCH_SYSTEM32`, or from Program Files only.
- **Clock domains**: `ph_clk` domains with higher `prio` win. AutoTDP uses the best CPU domain and the selected GPU domain. `util` (busy %) improves bottleneck detection.

## License

MIT. See [LICENSE](LICENSE).
