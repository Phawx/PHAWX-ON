# Phawx ON plugins

A plugin is a 64-bit Windows DLL that adds hardware support to Phawx ON: its own rows in the overlay, CPU or GPU clock domains that AutoTDP can drive, fans and RGB lights. It uses a plain C interface, so it can be written in C, C++, Rust or anything else that can export a C function. The whole interface is one header, [`plugins/sdk/phawx_plugin.h`](../plugins/sdk/phawx_plugin.h). The release zip includes it in `sdk\`, together with the example plugin's source.

- [How Phawx ON finds and loads plugins](#how-phawx-on-finds-and-loads-plugins)
- [Writing a plugin](#writing-a-plugin)
- [Building](#building)
- [API reference](#api-reference)
- [Rules](#rules)
- [The RyzenAdj plugin](#the-ryzenadj-plugin)
- [Troubleshooting](#troubleshooting)

## How Phawx ON finds and loads plugins

### Where plugins go

Put the DLL in the `plugins` folder next to `PhawxON.exe`:

```
C:\Program Files\PhawxON\
    PhawxON.exe
    plugins\
        mydevice.dll                 a single-file plugin
        ryzenadj\                    or a folder, for a plugin with its own DLLs
            ryzenadj.dll
            libryzenadj.dll
```

At startup Phawx ON looks at every `.dll` in `plugins\` and one folder level below it. A DLL counts as a plugin only if it exports `phx_plugin_init`. Phawx ON checks this by reading the file's export table from disk, without loading it, so no code in a plugin runs before you turn it on. Other DLLs, such as a plugin's dependencies, are ignored.

The file name, lower-cased, is the plugin's **id**, for example `mydevice`. The id is used in `phawx.ini`, so renaming the file resets the plugin's settings. Two plugins with the same id cannot both load; the second one found is ignored.

### Turning plugins on

Every plugin starts **off**. The overlay's **Plugins** page lists each discovered plugin with an on/off switch and its state:

| State | Meaning |
|---|---|
| Running | Loaded, and its controls are live. |
| Not running | Turned off. |
| Not running · *reason* | Turned on but not loaded. The reason comes from the host or from the plugin, e.g. "needs an AMD Ryzen APU" or "a DLL it needs is missing". |
| … starts / stops after a restart | You changed the switch. Changes take effect the next time Phawx ON starts. |
| Not running · turned off after it crashed Phawx ON | See [crash protection](#crash-protection). |

Turning a plugin on asks for confirmation, because a plugin runs inside Phawx ON with administrator rights and full hardware access. The state is saved in `phawx.ini`:

```ini
[plugins]
mydevice=1
```

After changing a switch, a **Restart Phawx ON to apply** row appears. It starts a new Phawx ON and exits the old one. The new instance waits until the old one has restored the hardware and quit, then reopens on the Plugins page.

Plugins are never loaded or unloaded while Phawx ON runs. Their controls and clock domains are wired into the UI, the profile system and AutoTDP, which keep pointers to them. That wiring happens once, at startup, before AutoTDP builds its list of clock domains.

### Program Files only

Plugins only run when Phawx ON is installed under `C:\Program Files` (or `Program Files (x86)`), where only administrators can write. Phawx ON runs as administrator, so from any other folder a normal program could drop a DLL into `plugins\` and have it run with those rights. Outside Program Files, plugins are still listed but show **Not running · install Phawx ON in Program Files to run plugins**. *Start with Windows* has the same rule.

### Loading, step by step

For each plugin that is turned on, in id order:

1. `phawx.ini` gets `[plugins] ~loading=<id>`.
2. The DLL is loaded with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`. DLLs the plugin links to are found in its own folder or in System32, never in `PATH` or the current directory.
3. `phx_plugin_init` is called. Everything the plugin registers is held back until init returns.
4. If init returns `PHX_OK`, the registrations take effect. Otherwise they are dropped and the DLL is unloaded.
5. The `~loading` marker is removed.

### Crash protection

- **During startup:** if a plugin crashes or hangs Phawx ON in step 1–5, the marker stays in `phawx.ini`. The next start turns that plugin off and says so on the Plugins page.
- **Later:** if a plugin's code crashes after it is running, Phawx ON's crash handler sees the fault is inside that DLL. It does not call the plugin again, restores Phawx ON's own hardware settings and the other plugins' settings, and marks the plugin to be turned off at the next start.

Plugins run in Phawx ON's process, so a crash in a plugin still ends Phawx ON. These measures only stop it from crashing again and again.

## Writing a plugin

A plugin exports one function:

```c
PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **info);
```

- `host` is the host API: a table of functions plus a description of the machine.
- `self` identifies your plugin; pass it back to every host function.
- `*info` is where you return your name, version and callbacks.

A minimal plugin with one switch:

```c
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phawx_plugin.h"

static const phx_host *H;
static phx_plugin *SELF;
static int on_hw;                         /* what we changed, so we can undo it */

static int PHX_CALL set_boost(phx_control *c, int32_t v)
{
    /* talk to the hardware here; return 0 on success */
    on_hw = v;
    return 0;
}

static phx_control boost = {
    .size = sizeof(phx_control), .type = PHX_TOGGLE, .key = "boost", .label = L"Turbo fan curve",
    .page = PHX_PAGE_SYSTEM, .flags = PHX_F_OPTIONAL | PHX_F_PROFILE, .set = set_boost,
};

static void PHX_CALL on_shutdown(void)
{
    if (on_hw) { /* put the hardware back */ }
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"My device", .version = L"1.0", .shutdown = on_shutdown,
};

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    H = host;
    SELF = self;
    if (host->version < PHX_API_VERSION) return PHX_E_VERSION;
    if (lstrcmpiW(host->platform->maker, L"MyVendor")) return PHX_UNSUPPORTED;   /* not this machine */
    if (host->add_control(self, &boost)) return PHX_ERROR;
    *out = &info;
    return PHX_OK;
}
```

[`plugins/example/example.c`](../plugins/example/example.c) is a complete plugin with a fan, RGB lights, a switch and an info row. It uses simulated hardware, so it is safe to try.

Use `#define WIN32_LEAN_AND_MEAN` before `<windows.h>`, or pick names other than `shutdown` for your callbacks: the full `<windows.h>` includes winsock, which declares a `shutdown()` of its own.

### Returning from init

| Return | Shown as | Use it when |
|---|---|---|
| `PHX_OK` | Running | Everything is registered. |
| `PHX_UNSUPPORTED` | Not running (grey) | The hardware is not present. |
| `PHX_ERROR` (or any negative value) | Not running (red) | Something is wrong that the user can fix. Call `host->set_status` first to say what. |
| `PHX_E_VERSION` | Not running · needs a newer Phawx ON | `host->version` is older than you need. |

On any return other than `PHX_OK`, undo whatever you did before returning: Phawx ON unloads the DLL.

## Building

The header is plain C and has no dependencies other than the C runtime.

**MinGW-w64** (what Phawx ON itself uses):

```sh
x86_64-w64-mingw32-gcc -shared -O2 -static-libgcc -Ipath/to/sdk mydevice.c -o mydevice.dll
```

**Visual Studio** (x64 Native Tools prompt):

```bat
cl /LD /O2 /Ipath\to\sdk mydevice.c
```

In the Phawx ON repository, `make plugins` builds the plugins under `plugins/` into `build/plugins/<name>/<name>.dll`. To add one, create `plugins/<name>/<name>.c` and `<name>.rc`, and add the name to `PLUGIN_NAMES` in the Makefile.

Add a version resource with a `FileDescription`. Phawx ON shows it as the plugin's name while the plugin is off, when it cannot run your code to ask. `plugins/example/example.rc` shows the layout.

To test, copy the DLL into `C:\Program Files\PhawxON\plugins\`, turn it on and restart Phawx ON. Set the environment variable `PHAWX_LOG=1` to get `phawx.log` next to the exe. Your `host->log` lines appear there with your id in front.

The header checks the size of every struct at compile time. If one of those checks fails, the compiler is laying the struct out differently from Phawx ON. Usually that means a packing option such as `/Zp1`; the header pins packing to 8 itself.

## API reference

The comments in `phawx_plugin.h` are the authoritative reference. This section covers how the pieces fit together.

### Controls: `phx_control`

A control is one row in the overlay. Register it with `host->add_control`. The host draws it, handles gamepad, touch and mouse input, and saves the value. It restores the value at the next start, switches it with per-game profiles (with `PHX_F_PROFILE`), and lets the user pin the row to the Quick page and hold to reset it.

| Type | What set() receives |
|---|---|
| `PHX_SLIDER` | `min`..`max` in steps of `step`. `fmt` can format the value (for example mW as "15 W"). |
| `PHX_TOGGLE` | 0 or 1. |
| `PHX_CHOICE` | An index into `choices` (a NULL-terminated array of labels). |
| `PHX_ACTION` | Called when the row is pressed. |
| `PHX_INFO` | Read-only. Give it `get` (value) and/or `fmt` (text). Refreshed about once a second while the overlay is open. |
| `PHX_HEADER` | A section title. If you add rows to a page without a header, Phawx ON adds one with your plugin's name. |

- **Keys.** `key` is relative to your plugin: `"stapm"` is stored as `ryzenadj.stapm` in the `[global]` or `[game:<exe>]` sections. Use letters, digits, `_`, `-` and `.`, at most 40 characters. Headers and info rows need no key.
- **Order.** `order` (0..999) orders your rows on a page. Plugin rows always come after Phawx ON's own rows, and each plugin's rows stay together.
- **`def` and Default.** With `PHX_F_OPTIONAL`, the row has a **Default** position that means "Phawx ON leaves this alone". It starts there, and returning to it (hold X or R, or a 2-second touch) does not call `set()`. Without the flag, Default means `def`, and returning to it calls `set(def)`. Have `get()` read the hardware, so the row can show "Default (15 W)".
- **`value` and `active`.** The host keeps these up to date before every call into your plugin. `active` is 0 while the row is at Default.
- **Changing a control later.** After changing `label`, `unit`, `flags`, `min`/`max`/`step`/`def` or `choices` of a registered control, call `host->update(self, &control)`. For example, set `PHX_F_HIDDEN` when a feature turns out to be missing.

Every control that is away from Default is applied again after sleep and when the machine switches between AC and battery, because firmware often resets limits then.

Flags: `PHX_F_NOSAVE`, `REAPPLY` (marks values the firmware resets on its own), `OPTIONAL`, `ADVANCED` (hidden unless Settings > Show advanced options), `DANGER` (asks before applying), `LIVE` (applied while a slider is dragged), `HIDDEN`, `AUTOTDP` (locked while AutoTDP drives your clock), `PROFILE`, `SIGNED`.

### Clock domains: `phx_clock`

`host->add_clock` registers a CPU (`PHX_CLOCK_CPU`) or GPU (`PHX_CLOCK_GPU`) clock that AutoTDP can raise and lower between `min_mhz` and `max_mhz`.

- **Which domain is used.** For the CPU, AutoTDP uses the domain with the highest `prio`. Phawx ON's own domains use 10–30. GPU domains appear under **GPU clock control**, where the user picks one.
- **Callbacks.** `set_max` caps the clock and `reset` removes the cap. `set_min`, `cur` (current MHz) and `util` (busy %) are optional; `util` improves AutoTDP's bottleneck detection.
- **Threading.** These callbacks run on AutoTDP's worker thread. Keep each call well under 50 ms.

### Fans: `phx_fan`

Give the host three functions and it provides the rows: a *Fan mode* row (Default / Auto / Manual / Full speed), a *Manual fan speed* slider from `min_pct` to 100 %, and *Fan speed* in RPM if you provide `get_rpm`. They appear on the System page under `name`, with the keys `<id>.<fan id>.mode` and `.speed`.

The host calls `set_auto` when the user picks Default, when Phawx ON exits, and from the crash handler. You never leave a fan in manual mode by accident.

### RGB lights: `phx_rgb`

The host provides three rows. *Lighting* offers Default / Off / Solid plus your `effects` (up to 8). *Color* offers 9 named colors if `caps` has `PHX_RGB_COLOR`, and *Brightness* offers 0–100 % if it has `PHX_RGB_BRIGHTNESS`. It calls `apply` with the whole state each time one of them changes:

```c
typedef struct phx_rgb_state {
    int32_t mode;          /* PHX_RGB_OFF, PHX_RGB_SOLID, or 2 + index into effects */
    uint8_t r, g, b, reserved;
    int32_t brightness;    /* 0..100 */
} phx_rgb_state;
```

`restore` hands the lights back to the firmware. It is called when the user picks Default, at exit and from the crash handler.

### Host functions

| Function | |
|---|---|
| `add_control`, `add_clock`, `add_fan`, `add_rgb` | Register. Only valid inside `phx_plugin_init`. Return 0 or `PHX_ERROR`; the reason goes to the log. |
| `update` | Re-read a control after you changed it. |
| `cfg_get_int`, `cfg_set_int`, `cfg_get_str`, `cfg_set_str` | Your own `[plugin:<id>]` section in `phawx.ini`, for settings that are not controls. Keys follow the control key rules. Values cannot contain line breaks. |
| `log` | One line to the log. `phx_logf(host, self, fmt, ...)` formats with your own C runtime first. |
| `toast` | A short message at the bottom of the screen. |
| `set_status` | A line shown under your name on the Plugins page: why you are not running, or a note while you are. |
| `plugin_dir` | The folder your DLL was loaded from. Load your own dependencies by full path from here. |
| `ctl_get` | Any control's value and state by its full key (`"cpu.tdp"`, `"example.fan.mode"`). Returns -1 if it does not exist or is hidden. |
| `hw_ready` | 1 if PawnIO is loaded. |
| `ec_read`, `ec_write` | One byte of the ACPI embedded controller (ports 0x62/0x66) through PawnIO's signed LpcACPIEC module, under the `Global\Access_EC` lock that other tools share. |
| `msr_read`, `msr_write` | A model-specific register through PawnIO's signed modules. Only the registers those modules allow will work. `cpu` -1 means any CPU. |
| `platform` | CPU vendor, family, model and stepping, core counts, and the SMBIOS maker, product and board strings to match your device against. |

### Threading

- Phawx ON calls you on its UI thread; clock callbacks run on AutoTDP's thread. It never runs two of your callbacks at the same time, so you do not need your own lock for state they share.
- Call the host back from the thread it called you on. `log`, `ec_*` and `msr_*` may be called from any thread.
- `tick` runs about once a second, and only while the overlay is open. Keep it short: the overlay waits for it.

## Rules

- **Undo everything in `shutdown`.** It runs when Phawx ON exits, on logoff, and from the crash handler, possibly on another thread. Phawx ON's promise is that its changes do not outlive it, and that includes yours.
- **Start at Default.** Use `PHX_F_OPTIONAL` for anything that changes hardware, so nothing changes until the user asks.
- **Go through PawnIO or the vendor's own API.** Do not bring an unsigned or known-vulnerable kernel driver of your own. `ec_*` and `msr_*` cover most handheld fan and LED controllers. The RyzenAdj plugin is an exception for machines without PawnIO, and says so.
- **Keep what you register valid until the process exits.** Structs, strings, choice lists: use static storage. Phawx ON never unloads a running plugin.
- **Do not let C++ exceptions or `longjmp` cross into Phawx ON**, and never free memory the other side allocated.
- **Keep callbacks fast.** Most run on the UI thread, and clock callbacks sit in AutoTDP's loop.
- **Check `host->version`**, and return `PHX_E_VERSION` if it is older than you need. Newer hosts only append to the API, so a plugin built for version 1 keeps working.

## The RyzenAdj plugin

`plugins\ryzenadj\ryzenadj.dll` ships with Phawx ON and is off by default. It controls AMD APU power limits through [RyzenAdj](https://github.com/FlyGoat/RyzenAdj)'s `libryzenadj.dll`, for machines where Phawx ON's own SMU control (through PawnIO) does not work.

- **Setup:** copy `libryzenadj.dll` and the files that come with it in RyzenAdj's Windows release (`WinRing0x64.dll`, `WinRing0x64.sys`, `inpoutx64.dll`) into `plugins\ryzenadj\`. Then turn the plugin on and restart.
- **Controls:** on the CPU page, TDP (sustained), Boost TDP (fast), Slow limit (advanced) and Temperature limit, plus APU power and temperature readouts. On Raven, Picasso, Dali and Lucienne it also gives AutoTDP an iGPU clock domain.
- **When it stands down:** if Phawx ON's own AMD TDP control is working, the plugin does not start ("not needed"), so the two never fight over the same limit.
- **Restoring:** it puts back the limits it found when Phawx ON exits, if the firmware's power table was readable.
- **Locking:** it takes `Global\Access_PCI` around every call, like other tools that share the SMU.
- **Driver risk:** libryzenadj reaches the hardware through the WinRing0 and inpoutx64 drivers. Microsoft's vulnerable driver blocklist and Defender block these on many systems, and while they are loaded other programs can use them too. Phawx ON never turns off the blocklist or Memory Integrity to make this work. If the driver is blocked, the plugin shows *RyzenAdj could not open its driver* and stays off. Prefer PawnIO where it works.

## Troubleshooting

| Symptom | Fix |
|---|---|
| The plugin is not listed | It must be a DLL in `plugins\` or one folder below, exporting `phx_plugin_init` with that exact name (use `extern "C"` in C++). |
| *install Phawx ON in Program Files to run plugins* | Move the Phawx ON folder under `C:\Program Files`. |
| *a DLL it needs is missing* | Something the plugin links to is not in its folder or in System32 (for example `vcruntime140.dll` for an MSVC build: install the VC++ runtime, or link statically). |
| *built for 32-bit Windows* | Build for x64. |
| *not a Phawx ON plugin* | The export is missing or misnamed. |
| *turned off after it crashed Phawx ON* | Fix the crash, then turn the plugin back on. Run with `PHAWX_LOG=1` to see how far it got. |
| Rows missing | Check `phawx.log` for "rejected": a bad key, a missing `set()`, a choice row without choices, or a registration made after init returned. |
