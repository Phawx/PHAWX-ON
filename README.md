# Phawx ON

Phawx ON is a small, open-source performance tuning app for Windows handhelds and laptops. It is a native C tray app of about 330 KB (PawnIO modules included) with no runtime dependencies. It adds a Steam Deck–style quick-access overlay where you set TDP, EPP, CPU and GPU clocks, and hybrid P/E core behaviour. It also has an **AutoTDP** mode that holds a target frame rate with as little power as it can: the lowest CPU and GPU clocks, the fewest unparked cores and the laziest EPP that still reach it, checked against what the machine actually draws.

It is built to be a good citizen:

- Hardware access goes only through the **signed PawnIO driver and its signed modules**. The one exception is the optional RyzenAdj plugin, which is off unless you turn it on (see [Plugins](#plugins)).
- Frame data comes only from **Intel PresentMon**. It does not inject into or hook games.
- The overlay appears only when you ask for it.
- It never turns off security features: no HVCI, driver blocklist, VBS or Defender changes.
- No undervolting.
- Changes are reverted when the app exits or crashes, and reapplied after sleep.

See [docs/FEATURES.md](docs/FEATURES.md) in the repository for the full feature matrix, including what is not supported and why.

> Hardware behaviour cannot be tested in CI. Every hardware control starts at **Default**, which means Phawx ON leaves that setting alone until you change it.

## Features

- **Overlay.** Slides in from the right edge, and gamepad, touch and mouse all work in it. The header reads **Phawx ON** (green) while AutoTDP runs and **Phawx OFF** otherwise, with live FPS and clocks below it. Pages: Quick, CPU, GPU, Display, System, Plugins, Settings. System has the sections Power, Fan control, Lighting, Windows and Hardware info.
- **Pins.** Every setting and action has a pin, except those on the Quick page itself and the Plugins page's own switches and links. Pinned ones appear on the Quick page, below its own rows.
- **Hold to reset.** Hold X or R on a setting for half a second (or touch and hold it for 2 seconds) to put it back to Default.
- **Fan control.** Auto, Manual, Full speed or **Curve** for the built-in GPD Win Mini fan and for plugin fans. In Curve mode you draw the fan speed against the CPU temperature on a graph.
- **Lighting.** RGB lights from plugins: off, solid colour or the plugin's effects, colour and brightness.
- **Hardware info.** A table of everything Phawx ON can read: system, BIOS and Windows version, CPU, load, power and temperature, memory, GPU clocks, load, power and temperature, display mode, fan speeds, battery and its discharge rate, and the state of PawnIO and PresentMon.
- **Plugins.** DLLs in a `plugins` folder add controls, clock domains, fans and RGB lights. The Plugins page also shows whether PawnIO and PresentMon are installed and running. A RyzenAdj plugin is included. The plugin guide is [docs/PLUGINS.md](docs/PLUGINS.md) in the repository and `sdk\PLUGINS.md` in the release zip.
- **TDP.**
  - Intel: PL1/PL2 and their time windows, through MSR 0x610 with the real power units.
  - AMD Ryzen APUs (Raven through Strix Halo): sustained, boost, slow and APU slow limits, time constants and temperature limit, through the SMU.
- **EPP.** Windows energy performance preference, set separately for P-cores and E-cores on hybrid CPUs.
- **Cores.** On the Quick page, how many cores Windows may keep unparked: **P-cores** and **E-cores** on a hybrid CPU, **Cores** on one with a single kind, as a count ("4 of 6"). The CPU page has the minimum unparked, per-class clock caps and the thread scheduling policy (prefer P or E cores).
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
  - Watches the foreground game's FPS through PresentMon, and detects whether the CPU or the GPU is the bottleneck.
  - Raises CPU or GPU clocks quickly when FPS drops, and lowers them slowly once FPS is stable.
  - Looks for the fewest cores Windows needs to keep unparked (per core type on hybrid CPUs), and for the highest EPP that still holds the frame rate.
  - Measures the power draw: the battery's discharge rate (the whole machine) on battery without a charger, the CPU package and a discrete GPU otherwise. Fewer cores or a higher EPP is kept only when the machine then draws less.
  - Target can be the refresh rate, half the refresh rate, or a custom value.
- **Per-game profiles** (off by default). Save the current settings for the game in front. They load automatically whenever that game is in the foreground.
- **Display.** Refresh rate and resolution.
- **System.** Game Mode, Game Bar capture, GPU hardware scheduling, touch keyboard, Modern Standby override, and GPD Win Mini fan control.

## Requirements

- Windows 10 21H2 or later, or Windows 11, x64. The app asks for administrator rights when it starts.
- **PawnIO**, needed for TDP, CPU clock, AMD iGPU clock and fan control:
  Install it from <https://pawnio.eu>. That installs the driver and `PawnIOLib.dll`, and nothing else is needed. PawnIO's installer ships no modules, so Phawx ON brings the signed ones it uses, from [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) 0.2.11 (LGPL-2.1), in `plugins\PawnIO\modules\` next to `PhawxON.exe`:
  - Intel: `IntelMSR.bin`, `IntelMCHBAR.bin`
  - AMD: `AMDFamily17.bin`, `RyzenSMU.bin`
  - Fan: `LpcACPIEC.bin`

  To update them, copy newer files from the [PawnIO.Modules releases](https://github.com/namazso/PawnIO.Modules/releases) into that folder; the `README.md` there explains how. Phawx ON loads each module from that folder first. If one is missing there, it uses the copy built into `PhawxON.exe`, and after that PawnIO's own folder (`C:\Program Files\PawnIO\` or its `modules\`).

  The driver only runs code signed by the PawnIO project. Phawx ON can therefore only do what those modules allow. On Intel this means the power limit (0x610) is available, but turbo ratios and HWP MSRs are not. Without PawnIO, the Windows power, GPU vendor API and display controls still work. The **Plugins** page shows whether PawnIO is running, with its version, and links to the download.
- **Intel PresentMon** (only needed for AutoTDP and the FPS readout): install PresentMon 2.x from <https://game.intel.com/story/intel-presentmon/> or [GitHub](https://github.com/GameTechDev/PresentMon/releases). Phawx ON uses the PresentMon service and its `PresentMonAPI2.dll`. It only loads that DLL from under Program Files, and starts the service if it is stopped.
- GPU clock control uses each vendor's own driver component: `amdadlx64.dll` / `atiadlxx.dll` (AMD Adrenalin), `nvml.dll` (NVIDIA driver) or `ControlLib.dll` (Intel graphics driver). Controls for a GPU without its driver component are hidden.

## Install

1. Download the latest `PhawxON-<version>.zip` from [Releases](https://github.com/Phawx/PHAWX-ON/releases).
2. Extract it to **`C:\Program Files\`**. The zip contains a `PhawxON` folder, so the app ends up in `C:\Program Files\PhawxON\PhawxON.exe`. "Start with Windows" and plugins only work under Program Files, because an app that runs elevated must never load anything from a folder that normal users can write to.
3. Install PawnIO, and PresentMon if you want AutoTDP (see above).
4. Run `PhawxON.exe`.
5. Optional: turn on **Settings → Start with Windows**. This creates a logon task that starts Phawx ON hidden in the tray (`/tray`).

Settings are stored in `phawx.ini` next to the exe, or in `%APPDATA%\PhawxON\` if that folder cannot be written. To get a debug log (`phawx.log` in the same folder), set the environment variable `PHAWX_LOG=1`.

To uninstall:
1. Turn off Start with Windows.
2. Exit Phawx ON from the tray. It restores every setting it changed.
3. Delete the folder.
4. Only if you turned on the RyzenAdj plugin's *Read power table*: remove the InpOut driver it installed (`sc delete inpoutx64` as administrator, restart, then delete `C:\Windows\System32\drivers\inpoutx64.sys`).

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
| Hold X (the left face button) or R on the keyboard, about half a second | Touch and hold for 2 seconds (mouse: half a second) | Reset the setting to **Default** (Phawx ON stops managing it). A bar under the row shows the progress. |
| Y (the top face button) | Tap the pin at the left of the row (on a slider, on its label line) | Pin the setting to the Quick page, or unpin it |
| On a fan curve: left/right picks a point, A starts editing it, up/down change it by 5 %, A or B ends | Drag a point up or down | Edit the fan curve |
| B | - | Close the menu |

Holding only resets saved settings. On a setting that is already at Default it just says *Already at Default*, and on a row locked by AutoTDP *Managed by AutoTDP*. On actions, links and the plugin switches a long press is an ordinary tap. A touch or click that moves more than about 10 pixels is not a tap or a hold: moving up or down scrolls, moving sideways cancels. Windows' own press-and-hold ring, pen feedback and flicks are turned off in the overlay.

Changes take effect about half a second after you stop adjusting. Settings marked dangerous ask for confirmation first. Advanced options are hidden until you turn on **Settings → Show advanced options**. **Settings → UI scale** makes the overlay smaller or larger (75–200 % of the monitor's own scaling).

The overlay never takes keyboard focus from the game, so while it is open Phawx ON watches the R key without taking it: the game sees R too. AutoTDP is turned on and off with the **Phawx ON** switch at the top of the Quick page, or from the tray icon.

### System page

- **Power:** Windows' minimum and maximum processor state, plus (advanced) autonomous mode, duty cycling, boost policy and latency hints. This used to be the Power tab.
- **Fan control:** every fan Phawx ON or a plugin can drive. **Fan mode** is Default (the firmware decides), Auto, Manual (the **Manual fan speed** slider), Full speed or **Curve**.
  - In **Curve** a graph appears with 7 points, the fan speed at 30, 40 ... 90 °C. Drag a point, or use the gamepad as in the table above. Moving a point drags its neighbours along so the curve always rises. The current temperature and speed are marked on the graph.
  - About once a second Phawx ON sets the fan from the CPU temperature (Intel package temperature, AMD Tctl, read through PawnIO). Between points it interpolates; at 95 °C or more the fan always runs at 100 %. It speeds up at once and slows down by at most 5 % a second, so it does not hunt.
  - Without a temperature for 5 seconds the fan goes back to the firmware until readings return. At exit, and after a crash, the firmware gets the fan back as with the other modes.
  - Hold X on the graph to go back to the default curve. The curve is saved in `phawx.ini` and is the same for every game.
- **Lighting:** RGB lights from plugins. Shown only when a plugin adds some.
- **Windows:** Game Mode, Game Bar capture and the other Windows options.
- **Hardware info:** a read-only table, refreshed about once a second while the overlay is open. It lists what the CPU, GPU, display and plugin backends read, plus system, memory, battery and software information. Sensors that are not available on your machine are left out.

### Pins

Pinned settings are listed on the Quick page under **Pinned**, in the order you pinned them, with the page and section they come from. They are saved in `phawx.ini` (`[pins]`), up to 64. A pin on a setting that is currently unavailable, such as one from a plugin that is off, is kept and comes back with the setting.

### AutoTDP

1. On the **Quick** page, turn on **Phawx ON**.
2. Choose a target: refresh rate, half refresh, or custom FPS.
3. Start a game.

The status line shows current and target FPS, the CPU and GPU clocks, and which one is the bottleneck. **Power use** below it shows what the machine draws (*system* on battery, *CPU+GPU* otherwise), how many cores are unparked, the EPP, and what AutoTDP is trying at the moment. While AutoTDP runs, the clock, EPP and core controls it manages are locked, and their sliders follow it live: they show the value AutoTDP has set right now ("AutoTDP · 2400 MHz", "AutoTDP · 4 of 6") and the knob glides as it changes. The one it is trying at the moment turns amber and reads "Trying …". Controls AutoTDP is not managing stay yours: the cores when **Find the fewest cores** is off, EPP when it is left to Windows.

How it works:

- **Clocks first.** When FPS falls short it raises the CPU or GPU clock cap at once (both when it is far off). While FPS holds it lowers them a step at a time. Far from the limit these are quick steps watched for dropped frames. Near it, each step is kept unless the power reading goes up, which happens below the voltage floor, where a lower clock only makes the chip busy for longer.
- **Then cores and EPP**, one trial at a time, while the frame rate is steady. A trial parks one or more cores (**CPMAXCORES**, per core type on hybrid CPUs) or raises EPP by 20 (up to 60). The clocks may rise a little to make up for it. After it settles, AutoTDP compares the power with what it drew before. It keeps the change only if the machine now draws clearly less at the same frame rate and the 1% lows held, and otherwise puts back the setting and the clocks. A trial that saved nothing is tried again later with a bigger step, after 30 s, 1, 2 and then 4 minutes. A value that cost frames is left alone for 20 s, longer if it keeps failing.
- **When frames fall short** with cores parked or EPP raised (and the GPU is not the limit), it goes back toward all cores and EPP 0 at once, all the way if it is far off.
- **Floors.** At least two cores stay unparked, and on a hybrid CPU at least two P-cores and one E-core. It never allows more cores than your own **P-cores** / **E-cores** / **Cores** settings (or the power plan) already do. Phawx ON reads which cores Windows has really parked, so it skips limits that change nothing and adjusts to how Windows rounds the percentage.
- **Timing.** A package reading settles in a second. A battery that only updates every few seconds makes each trial wait for a few updates. Without any power reading AutoTDP only parks cores where the GPU is clearly the limit and the CPU mostly idles, and leaves EPP at 0.
- **Out of reach.** If the target cannot be reached even at full speed, AutoTDP holds what the machine does reach, a little under it, with the least power. Now and then it checks whether the target has become reachable again.
- **No game.** The clock caps come off, and core parking and a tuned EPP go back to Windows' settings. If the same game shows frames again within a minute (a pause, alt-tab), the cores and EPP go back to where the search was. Everything is restored when AutoTDP stops, when Phawx ON exits, and after a crash.

Under **CPU → AutoTDP tuning** (advanced):

- **Find the fewest cores**: On by default (CPUs with three cores or more).
- **EPP while active**: *Tune for power* (the default) starts at 0 and looks for a higher EPP. *0 (fastest)* keeps EPP at 0 while AutoTDP runs, as 1.2 did. *Windows* leaves EPP alone.
- Floors, ceilings, aggressiveness and settle time are there too.

AutoTDP works on clocks, not power limits. Set a TDP ceiling on the Quick page if you also want a hard power cap.

`make sim` runs AutoTDP against a simulated handheld (`tests/autotdp_sim.c`, under wine off Windows). The scenarios include a GPU-bound game, a CPU-bound game, a scene change, a target out of reach, battery-only power and no power reading. It checks that the frame rate holds, that the power ends close to the best any setting could reach, and that every setting is restored.

### Per-game profiles

Per-game profiles are off until you turn on **Settings → Per-game profiles**, or save your first profile:

1. With the game in front, open the overlay.
2. Set it up as you like.
3. Go to **Settings → Save profile for current game**. This also turns per-game profiles on.

Phawx ON switches profiles when the game gains focus and switches back to your global settings when it loses focus. If you saved profiles with Phawx ON 1.0, profiles stay on after the update.

### Plugins

The **Plugins** page starts with the status of the two components Phawx ON relies on:
- PawnIO, for hardware access;
- PresentMon, for FPS.

Each shows *… is loaded and running* in green, or a red ✗ with what is wrong (*PawnIO driver is not running*, *PawnIO modules are missing*, *PresentMon service is not running*, *… is not installed*) and a **Download** link to the official site. *Checking PresentMon…* shows for a moment after Phawx ON starts.

Below that is every plugin found in the `plugins` folder next to `PhawxON.exe`, with an on/off switch and a status line: *Running*, *Not running*, or *Not running · …* with the reason. Plugins are off until you turn them on; turning one on asks first, because plugins run with administrator rights and full hardware access. Changes apply at the next start: use **Restart Phawx ON to apply**. Plugins only run when Phawx ON is under Program Files; anywhere else they show *install Phawx ON in Program Files*. A plugin that crashes or hangs Phawx ON is turned off automatically at the next start, and Phawx ON still restores the hardware. To write one, see the plugin guide: [docs/PLUGINS.md](docs/PLUGINS.md) in the repository, `sdk\PLUGINS.md` in the zip, next to the SDK header and an example plugin.

The zip includes the **RyzenAdj** plugin (`plugins\ryzenadj\`), off by default. It sets AMD APU power limits through RyzenAdj's `libryzenadj.dll` on machines where Phawx ON's own SMU control through PawnIO does not work, and stands down where it does. It needs RyzenAdj's Windows release files copied next to it. Be aware of what it brings:
- It uses the WinRing0 driver instead of PawnIO. While the plugin runs, **any program on this PC can use that driver to take full control of it.** Windows may block the driver.
- Its optional *Read power table* setting also installs the InpOut driver, which has the same problem, **stays installed after Phawx ON exits** and loads at every boot until you remove it.
- Without *Read power table* (or on CPUs whose table cannot be read), the limits it sets are not read back or restored when Phawx ON exits.

See the RyzenAdj section of the plugin guide for setup, the risks and how to remove InpOut.

## Build

You need `x86_64-w64-mingw32-gcc` and `windres` from mingw-w64 (Debian/Ubuntu: `apt install mingw-w64`, or MSYS2 on Windows) and GNU make.

```sh
make -j8        # build/PhawxON.exe and the plugins in build/plugins/
make dist       # dist/PhawxON-<version>.zip: PhawxON\ with plugins\ryzenadj and sdk\
make clean
```

The build uses `-Wall -Wextra` and should produce no warnings. The version is set in `src/version.h`; `res/phawx.manifest` repeats it, and `make dist` fails if the two differ. `.github/workflows/release.yml` builds the zip and publishes the GitHub release `v<version>` with `RELEASE_NOTES.md` when you run it manually on `main`. It refuses to publish if that tag already exists on a different commit.

## Adding a hardware backend

Hardware support can be added as a [plugin](docs/PLUGINS.md), a separate DLL built and shipped on its own, or as a built-in backend. A backend is one C file that controls some hardware. The UI draws its controls automatically, profiles save them, and AutoTDP can drive its clocks. Adding one takes three steps:

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
- **`resume`** (optional): runs after sleep. Every control away from Default is then applied again automatically (also on AC/DC changes and profile switches), so `set` must be idempotent.
- **`tick`** (optional): runs about once a second while the menu is open, and after every such re-apply. Keep it cheap.
- **Hardware access**: go only through `drv_*` (PawnIO) or an official vendor API. Load vendor DLLs with `LOAD_LIBRARY_SEARCH_SYSTEM32`, or from Program Files only.
- **Clock domains**: `ph_clk` domains with higher `prio` win. AutoTDP uses the best CPU domain and the selected GPU domain. `util` (busy %) improves bottleneck detection.

## License

MIT. See [LICENSE](LICENSE).
