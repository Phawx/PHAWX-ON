# Phawx ON plugins

A plugin is a 64-bit Windows DLL that adds hardware support to Phawx ON: its own rows in the overlay, CPU or GPU clock domains that AutoTDP can drive, fans and RGB lights. It uses a plain C interface, so it can be written in C, C++, Rust or anything else that can export a C function.

You need three files. They sit in different places in the repository and in the release zip:

| File | In the repository | In the release zip |
|---|---|---|
| `phawx_plugin.h`, the whole interface | `plugins/sdk/` | `sdk\` |
| `example.c`, a complete plugin with simulated hardware | `plugins/example/` | `sdk\` |
| `example.rc`, its version resource | `plugins/example/` | `sdk\` |
| This guide | `docs/PLUGINS.md` | `sdk\PLUGINS.md` |

The comments in `phawx_plugin.h` are the reference for every member. This guide explains how the pieces fit together.

- [How Phawx ON finds and loads plugins](#how-phawx-on-finds-and-loads-plugins)
- [Crash and hang protection](#crash-and-hang-protection)
- [Writing a plugin](#writing-a-plugin)
- [Building](#building)
- [API reference](#api-reference)
- [Loading your own DLLs](#loading-your-own-dlls)
- [Rules](#rules)
- [The RyzenAdj plugin](#the-ryzenadj-plugin)
- [Troubleshooting](#troubleshooting)

## How Phawx ON finds and loads plugins

### Where plugins go

Put the DLL in the `plugins` folder next to `PhawxON.exe`, or in a folder of its own inside it:

```
C:\Program Files\PhawxON\
    PhawxON.exe
    plugins\
        mydevice.dll                 a single-file plugin
        ryzenadj\                    or a folder, for a plugin with its own DLLs
            ryzenadj.dll
            libryzenadj.dll
```

The **Open plugins folder** row on the Plugins page opens (and if needed creates) this folder.

### What counts as a plugin

At startup Phawx ON looks at every file ending in `.dll` in `plugins\` and in the folders directly inside it (one level, not deeper). A file is listed as a plugin only if:

- its export table contains the name `phx_plugin_init`, spelled exactly like that (see [C++](#c) for `extern "C"`). Phawx ON reads the export table from the file on disk without loading it, so no code in a plugin runs before you turn it on;
- it is a DLL between 512 bytes and 64 MB;
- it is not a symbolic link or junction, and not inside one (those are skipped);
- no plugin with the same id was found before it (the second one is skipped and the log says so);
- fewer than 16 plugins were found before it. Discovery stops at 16.

Every other DLL, such as a plugin's own dependencies, is ignored. A 32-bit DLL (or one for any architecture other than x64) is listed, but it can never run.

### The plugin id

The id comes from the file name: everything up to its first dot, lower-cased, with every character other than `a`–`z`, `0`–`9` and `-` replaced by `_`, cut to 31 characters. `MyDevice.dll` becomes `mydevice`, `Legion Go.v2.dll` becomes `legion_go`.

The id is the prefix of every key your plugin stores in `phawx.ini` (`mydevice.boost`, `[plugin:mydevice]`), so renaming the file resets its settings.

These ids are reserved, because Phawx ON's own keys start with them: `amdgpu`, `app`, `arc`, `auto`, `autotdp`, `cpu`, `dev`, `display`, `gpu`, `input`, `intelgpu`, `nvidia`, `power`, `radeon`, `sys`. A plugin with one of these ids is listed as *its file name is reserved, rename it* and never loads.

### The name and warning shown before it runs

Until a plugin runs, Phawx ON cannot ask it for its name, so it reads the DLL's version resource (as data, without running it):

- **`FileDescription`** is the plugin's name on the Plugins page and in the question asked before turning it on. Without it, the id is shown. Once the plugin runs, the `name` in its `phx_info` replaces it.
- **`PhawxWarning`** (optional) is added to that question. Use it to say what the plugin brings with it, for example a driver. Phawx ON reads up to 319 characters, and the whole question is cut at 511.

Both come from the first language listed in `VarFileInfo\Translation`. `example.rc` shows the layout; add the warning to the same block as `FileDescription`:

```
    BLOCK "040904B0"
    BEGIN
      VALUE "FileDescription", "My device"
      VALUE "PhawxWarning", "Uses the XYZ driver, which any program on this PC can use while the plugin runs."
      ...
```

### Turning plugins on

Every plugin starts **off**. The Plugins page lists each discovered plugin with an on/off switch and a status line (see [Troubleshooting](#troubleshooting) for every status).

Turning a plugin on asks first: *Turn on My device (mydevice.dll)? Plugins run with administrator rights and full hardware access.*, followed by its `PhawxWarning`. Turning it off does not ask. The switch is saved in `phawx.ini`:

```ini
[plugins]
mydevice=1
```

Plugins are never loaded or unloaded while Phawx ON runs, because their controls and clock domains are wired into the UI, the profile system and AutoTDP. A change therefore applies at the next start:

- The status changes to *starts after a restart* or *stops after a restart*, with the toast *Takes effect when Phawx ON restarts*.
- A **Restart Phawx ON to apply** row appears. It starts a new Phawx ON and closes the old one. The new one waits (up to 15 seconds) until the old one has restored the hardware and quit, then opens on the Plugins page.

The row and the toast only appear for changes that actually wait for a restart: turning on a plugin that can load, or turning off one that is running.

### Program Files only

Plugins only run when `PhawxON.exe` is under `C:\Program Files` or `C:\Program Files (x86)` (the real Program Files folders, not `%ProgramFiles%`), where only administrators can write. Phawx ON runs as administrator, so from any other folder a normal program could drop a DLL into `plugins\` and have it run with those rights.

Outside Program Files, plugins are still listed, but every one of them, on or off, shows **Not running · install Phawx ON in Program Files**, and switching one on shows no restart prompt. *Start with Windows* has the same rule.

### Loading, step by step

At startup, after Phawx ON's own hardware backends, for each plugin that is turned on and can run, in id order:

1. The start marker `[plugins] ~loading=<id>` is written to `phawx.ini`.
2. The DLL is loaded with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`: DLLs it links to are found in its own folder or in System32, never in `PATH` or the current directory. Its `DllMain` runs.
3. `phx_plugin_init` is called. Everything the plugin registers is held back until it returns.
4. If it returns `PHX_OK`, its `phx_info` is copied and its rows, clock domains, fans and lights are registered. Otherwise they are dropped and the DLL is unloaded.
5. The marker is removed.

Then Phawx ON registers AutoTDP's and the Settings page's rows, loads the saved settings and applies them (`set()` for every saved value, then `tick`). The marker names each plugin again while it is being called during this first apply.

Loading is also where a plugin can be refused. The status line then says why: see [Troubleshooting](#troubleshooting).

## Crash and hang protection

Plugins run inside Phawx ON's process, so a crash in a plugin still ends Phawx ON. These measures make sure the hardware is put back, and that the same plugin does not crash Phawx ON again and again.

### During startup

From step 1 above until the first apply of saved settings has finished, the start marker names the plugin Phawx ON is inside: while its DLL loads, during `phx_plugin_init`, while a failed plugin is unloaded, and during every call into it in the first apply. If Phawx ON crashes or hangs there (or is killed while it hangs), the marker stays in `phawx.ini`. The next start logs *plugins: mydevice crashed or hung Phawx ON last time, turning it off*, turns the plugin off and shows **Not running · turned off after it crashed Phawx ON**. Turning it back on clears that.

A crash during startup that is not the plugin's fault (see below) clears the marker instead, so only the plugin that crashed is turned off.

### While running

When an unhandled exception happens, Phawx ON blames a plugin if:

- the fault is in the plugin's DLL;
- the fault is in any DLL loaded from the plugin's own folder `plugins\<name>\`, such as a library it ships with (this does not apply to plugins directly in `plugins\`);
- the faulting thread was inside a call from Phawx ON into that plugin, wherever the fault is: in the C runtime, in Windows, or a C++ exception;
- the plugin's code (or a DLL from its folder) is found further up the faulting thread's stack, as on the plugin's own worker threads when they fault inside the C runtime.

After a stack overflow only the first and third rules are checked.

A plugin that is blamed is marked as crashed: Phawx ON never calls it again, not even its `shutdown`, `set_auto`, `restore` or clock `reset`, because that code may be what failed. The marker names it (several plugins are listed as `a,b`), so the next start turns it off.

Then the crash handler starts a thread that restores the hardware, and waits up to 4 seconds for it:

1. AutoTDP stops driving Phawx ON's own clock domains and puts back EPP and core parking.
2. Phawx ON's own backends restore their settings.
3. Every plugin that did not crash, in reverse id order: clock domains it had capped are reset, fans are handed back to the firmware (`set_auto`), lights too (`restore`), then its `shutdown` runs.
4. The PawnIO driver is closed.

The log says *crash at <address>, in plugin mydevice* (or *not in a plugin*) and *crash: hardware restored*. Then Phawx ON ends. If a second plugin faults while this runs, Phawx ON ends at that point; that plugin is turned off at the next start too.

While restoring after a crash, a thread may still be stuck inside a plugin callback. Phawx ON waits at most about 300 ms for it, then makes its calls into that plugin anyway.

### Hangs

A hang is only caught during startup. After that, a callback that never returns freezes the overlay (or AutoTDP) until it does. Keep callbacks short, and put anything slow on your own thread with a timeout, as the RyzenAdj plugin does.

### Exit

On a normal exit, a logoff or shutdown, and **Restart Phawx ON to apply**, the order is the same as after a crash: AutoTDP stops, Phawx ON's own backends restore their settings, then the plugins in reverse id order (clock domains reset, fans and lights handed back, `shutdown`), and last the PawnIO driver is closed. `ec_*` and `msr_*` therefore still work in `shutdown`. If a plugin faults during exit, everything else is still restored and that plugin is turned off at the next start.

## Writing a plugin

A plugin exports one function, declared in `phawx_plugin.h`:

```c
PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **info);
```

- `host` is the host API: a table of functions plus a description of the machine.
- `self` identifies your plugin; pass it back to every host function.
- `*info` is where you return your name and callbacks.

A minimal plugin with one switch:

```c
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phawx_plugin.h"

#define NEEDS_API 1                       /* the oldest host API this plugin works with */

static const phx_host *H;
static phx_plugin *SELF;
static int changed;                       /* what we changed, so we can undo it */

static int PHX_CALL set_boost(phx_control *c, int32_t v)
{
    (void)c;
    /* talk to the hardware here; return 0 on success */
    changed = 1;
    return 0;
}

static phx_control boost = {
    .size = sizeof(phx_control), .type = PHX_TOGGLE, .key = "boost", .label = L"Turbo fan curve",
    .page = PHX_PAGE_SYSTEM, .flags = PHX_F_OPTIONAL | PHX_F_PROFILE, .set = set_boost,
};

static void PHX_CALL on_shutdown(void)
{
    if (changed) { /* put the hardware back */ }
}

static const phx_info info = {
    .size = sizeof(phx_info), .name = L"My device", .version = L"1.0", .shutdown = on_shutdown,
};

PHX_EXPORT int PHX_CALL phx_plugin_init(const phx_host *host, phx_plugin *self, const phx_info **out)
{
    H = host;
    SELF = self;
    if (host->version < NEEDS_API) return PHX_E_VERSION;
    if (lstrcmpiW(host->platform->maker, L"MyVendor")) return PHX_UNSUPPORTED;   /* not this machine */
    if (host->add_control(self, &boost)) return PHX_ERROR;
    *out = &info;
    return PHX_OK;
}
```

`example.c` is a complete plugin with a fan, RGB lights, a switch and an info row. It uses simulated hardware, so it is safe to try.

Use `#define WIN32_LEAN_AND_MEAN` before `<windows.h>`, or pick names other than `shutdown` for your callbacks: the full `<windows.h>` includes winsock, which declares a `shutdown()` of its own.

Compare `host->version` with the oldest version you need, not with `PHX_API_VERSION`. Otherwise a plugin rebuilt with a newer SDK refuses to run on older Phawx ON versions even if it uses nothing new.

### Returning from init

| Return | Status line | Use it when |
|---|---|---|
| `PHX_OK` | Running | Everything is registered. |
| `PHX_UNSUPPORTED` (1) | Not running · not for this device (grey) | The hardware is not present. |
| `PHX_E_VERSION` (-2) | Not running · needs a newer Phawx ON (red) | `host->version` is older than you need. |
| `PHX_ERROR` (-1), or any other value | Not running · failed to start (*n*) (red) | Something is wrong that the user can fix. |

If you called `host->set_status` before returning, your text replaces the host's reason, so say what is wrong: *needs an AMD Ryzen APU*, *libryzenadj.dll is missing from plugins\ryzenadj*.

On any return other than `PHX_OK`, undo whatever you did before returning, and stop any threads you started: Phawx ON drops your registrations and unloads the DLL.

## Building

The header is plain C, needs nothing but the C runtime and checks the size of every struct at compile time. If one of those checks fails, the compiler lays a struct out differently from Phawx ON. The header sets packing itself and no struct has implicit padding, so packing options such as `/Zp1` cannot cause this.

The commands below build the example in the zip's `sdk\` folder, where `phawx_plugin.h`, `example.c` and `example.rc` sit side by side. For your own plugin, replace `example` with its name; if the header is elsewhere, change `-I.` / `/I.` to its folder. In the repository, `make plugins` builds the plugins under `plugins/` into `build/plugins/<name>/<name>.dll`; to add one, create `plugins/<name>/<name>.c` and `<name>.rc` and add the name to `PLUGIN_NAMES` in the Makefile.

### MinGW-w64 (what Phawx ON itself uses)

```sh
x86_64-w64-mingw32-windres -O coff example.rc -o example.res.o
x86_64-w64-mingw32-gcc -shared -O2 -static-libgcc -I. example.c example.res.o -o example.dll
```

The DLL then imports only `KERNEL32.dll` and `msvcrt.dll`.

### C++

The header declares `phx_plugin_init` inside `extern "C"`, so a C++ definition gets the plain, undecorated export name without you writing `extern "C"`. A definition with a different return type does not compile, and one with different parameters fails to link (in C++ it would otherwise be a separate function with a decorated name, and the plugin would not be listed). With MinGW, link the C++ runtime statically, or the DLL needs `libstdc++-6.dll` and the plugin shows *a DLL it needs is missing*:

```sh
x86_64-w64-mingw32-g++ -shared -O2 -static -I. mydevice.cpp mydevice.res.o -o mydevice.dll
```

Code that includes the header but is not a plugin (a test program, for example) defines `PHX_HOST` before including it.

### Visual Studio (x64 Native Tools prompt)

```bat
rc /nologo example.rc
cl /LD /O2 /I. example.c example.res
```

`/LD` links the C runtime statically unless you add `/MD`. With `/MD` the plugin needs `vcruntime140.dll` from the Visual C++ Redistributable.

### Version resource

Always build the `.rc` file in: its `FileDescription` is the name the user sees before turning your plugin on, and `PhawxWarning` is your chance to warn them (see [above](#the-name-and-warning-shown-before-it-runs)). `example.rc` carries the example's names; change them for your plugin.

### Testing

Copy the DLL into `C:\Program Files\PhawxON\plugins\` (or a folder inside it), turn it on and restart Phawx ON. Set the environment variable `PHAWX_LOG` (for example to `1`) before starting Phawx ON to get `phawx.log` next to `phawx.ini`; your `host->log` lines appear there as `plugin <id>: ...`. They also always go to `OutputDebugString`, so a debugger or DebugView shows them without `PHAWX_LOG`.

## API reference

### Controls: `phx_control`

A control is one row in the overlay. Register it with `host->add_control` during `phx_plugin_init`. The host draws it, handles gamepad, touch and mouse input, saves the value, applies it again at the next start, switches it with per-game profiles (with `PHX_F_PROFILE`), and lets the user pin the row to the Quick page and hold it to go back to Default.

| Type | What `set()` receives |
|---|---|
| `PHX_SLIDER` | `min`..`max` in steps of `step`. |
| `PHX_TOGGLE` | 0 or 1. |
| `PHX_CHOICE` | An index into `choices`, a NULL-terminated array of up to 64 labels. |
| `PHX_ACTION` | Called when the row is pressed. |
| `PHX_INFO` | Read-only: give it `get` (a value) and/or `fmt` (text). Refreshed about once a second while the overlay is open. |
| `PHX_HEADER` | A section title. If you add rows to a page without a header of yours, Phawx ON adds one with your plugin's name. On the System page, which is made of sections, your plugin always gets a section of its own titled with its name, and your headers become the parts of it. |

- **Keys.** `key` is relative to your plugin: `"stapm"` is stored as `ryzenadj.stapm` in the `[global]` or `[game:<exe>]` section. Use 1 to 40 letters, digits, `_`, `-` and `.`. Sliders, toggles, choices and actions need a key and a `set()`; headers and info rows need neither. Keys must be unique within your plugin, including the keys of your fans and lights. Phawx ON compares keys exactly, but `phawx.ini` ignores case, so do not use two keys that differ only in case.
- **Order.** `order` (clamped to 0..999) orders your rows on a page. Plugin rows always come after Phawx ON's own rows and each plugin's rows stay together. The System page goes: Power, Fan control (every fan, built-in and from plugins), Lighting (every light), Windows, one section per plugin, Hardware info.
- **Pages.** `PHX_PAGE_POWER` is kept for old plugins, but there is no Power tab any more: those rows are shown on the System page, like `PHX_PAGE_SYSTEM` ones.
- **Hardware info.** Info rows (`PHX_INFO`) on the CPU, GPU, Display and System pages also appear in the Hardware info table at the bottom of the System page, with the same label and value.
- **Values.** `fmt` formats a value (for example mW as "15 W"); `n` is the size of the buffer in `wchar_t` including the terminator. With `fmt`, `unit` and `PHX_F_SIGNED` are ignored. Without it, the value is shown as a number with `unit`.
- **`value` and `active`.** The host writes these into your struct before every call into your plugin, so the struct must not be `const`. `active` is 0 while the row is at Default, and `value` is then `def`. Both change only after `set()` returns 0, so inside `set()` they still show the previous state.
- **Failure.** When a change the user made fails (`set()` returns non-zero), they see *Could not apply <label>*, unless the row is an action or you showed a toast of your own. Failures while saved values are applied again are silent.
- **`desc`.** For a `PHX_F_DANGER` row, the text of the confirmation, in place of *Set <label> to <value>? This can cause instability.* It can be long (the dialog grows to fit), and it is kept, not copied.
- **Changing a control later.** After changing `label`, `unit`, `desc`, `flags`, `choices`, the range or `def` of a registered control, call `host->update(self, &control)`. For example, set `PHX_F_HIDDEN` when a feature turns out to be missing. `update` may be called from any thread; off the UI thread the change is applied there a moment later, so leave the new values in the struct.

**Default, `def` and `PHX_F_OPTIONAL`.** Every row starts at Default. The user returns a row to Default by holding X or R, a 2-second touch (half a second with a mouse), or *Revert everything to defaults*.

- With `PHX_F_OPTIONAL`, Default means "Phawx ON leaves this alone". The row starts there and nothing is applied. Choosing Default, including holding X or R to reset the row, calls nothing in your plugin: the hardware keeps the last value it was given until Phawx ON exits or restarts (and then only goes back if your `shutdown` puts it back). Your plugin finds out only from `active` at its next call; if the hardware must go back sooner, do it in `tick` (which sees `active == 0`). Give the row a `get()` that reads the hardware, so it can show the current value. (Fans and lights are different: the host hands them back at once.)
- Without the flag, Default stands for `def`: returning to it calls `set(def)`, but a row at Default is not applied at start.

**Applied again.** Every row away from Default is applied again (`set()` with the same value) at start, after sleep, when the machine switches between AC and battery, and whenever a per-game profile is switched in or out. `set()` must therefore be idempotent. `PHX_F_REAPPLY` is reserved and has no effect.

**Flags**

| Flag | Effect |
|---|---|
| `PHX_F_NOSAVE` | Never written to or read from `phawx.ini`, so never applied at start or switched by profiles. |
| `PHX_F_REAPPLY` | Reserved, no effect (see above). |
| `PHX_F_OPTIONAL` | Has a Default position that leaves the hardware alone (see above). |
| `PHX_F_ADVANCED` | Only shown with **Settings → Show advanced options**. |
| `PHX_F_DANGER` | Red label, and asks for confirmation (`desc`) before `set()`, except when a toggle is turned off. Never applied while a slider is dragged, even with `PHX_F_LIVE`. |
| `PHX_F_LIVE` | `set()` while a slider is dragged (at most every 60 ms), not only on release. |
| `PHX_F_HIDDEN` | Not shown and not applied; `ctl_get` reports it as absent. |
| `PHX_F_AUTOTDP` | Locked (shown as *AutoTDP*) and not applied while AutoTDP runs, whichever clock domains it drives. Applied again when AutoTDP stops. |
| `PHX_F_PROFILE` | Saved per game when per-game profiles are on. |
| `PHX_F_SIGNED` | Positive values get a `+`. Ignored when `fmt` is set. |

### Clock domains: `phx_clock`

`host->add_clock` registers a CPU (`PHX_CLOCK_CPU`) or GPU (`PHX_CLOCK_GPU`) clock that AutoTDP can raise and lower between `min_mhz` and `max_mhz`.

- **Which domain is used.** For the CPU, AutoTDP uses the domain with the highest `prio` (clamped to 0..100; Phawx ON's own domains use 10–30). For the GPU, the user picks one under **Quick → GPU clock control**, which is listed only when there are two or more GPU domains; the highest `prio` is the default. The choice is saved by `name`.
- **Callbacks.** `set_max` (required) caps the clock. `reset` removes the cap. It is optional: without it the host calls `set_max(max_mhz)` instead, whenever the cap comes off (AutoTDP stops, **CPU max clock** or **GPU max clock** goes back to *Auto*, exit). `cur` (current MHz) feeds the overlay's live line while AutoTDP is off. `util` (busy %) improves AutoTDP's bottleneck detection. `set_min` is reserved and never called.
- **Range.** `0 <= min_mhz < max_mhz <= 20000`; `step_mhz` 0 means 50.
- **Threads.** `set_max` and `reset` run on AutoTDP's worker while AutoTDP runs, on the UI thread for the max clock rows, a GPU domain switch and when AutoTDP stops, and on the exit thread. The worker has a 64 KiB stack, so keep large buffers off the stack.
- **At exit and after a crash elsewhere**, the host resets every domain of yours that was capped (`reset`, or `set_max(max_mhz)` if `reset` is missing or fails) before your `shutdown`.

### Fans: `phx_fan`

Give the host three functions and it provides the rows in the Fan control section of the System page, under a header with `name`:

- **Fan mode:** Default / Auto / Manual / Full speed / Curve, key `<id>.<fan id>.mode`.
- **Manual fan speed:** `min_pct` to 100 % in steps of 5, key `<id>.<fan id>.speed`. Used only while the mode is Manual.
- **Fan curve:** a graph shown while the mode is Curve (see below).
- **Fan speed** in RPM, if you provide `get_rpm`.

`set_duty` receives `min_pct`..100 (Full speed is 100). The host calls `set_auto` when the user picks Auto, at once when the user picks Default (retried on every tick until it succeeds), and at exit and after a crash elsewhere, in both cases only if it had set a duty.

**Curve** needs nothing more from you. The user draws a curve of 7 points (fan % at 30, 40 ... 90 °C) and the host calls `set_duty` about once a second, on the UI thread, from the CPU temperature (Intel package temperature, AMD Tctl, both through PawnIO):

- Between two points the duty is interpolated. At 95 °C or more it is 100 %, whatever the curve says. It never goes below `min_pct`.
- The duty goes up at once; it comes down only after it would drop by 3 % or more, then by at most 5 % a second.
- The same duty is written again every 5 seconds, for embedded controllers that fall back to automatic.
- Without a CPU temperature for 5 seconds (no PawnIO, or it stops answering), the host calls `set_auto` and the firmware runs the fan until readings come back.
- The curve is saved as `[global] <id>.<fan id>.curve` (for example `20,25,35,50,65,85,100`). It is the same for every game. That key belongs to the fan, like `.mode` and `.speed`.

### RGB lights: `phx_rgb`

The host provides a header with `name` and three rows in the Lighting section of the System page:

- **Lighting:** Default / Off / Solid plus your `effects` (up to 8), key `<id>.<light id>.mode`.
- **Color:** 9 named colors, if `caps` has `PHX_RGB_COLOR`, key `.color`.
- **Brightness:** 0–100 % in steps of 10, if `caps` has `PHX_RGB_BRIGHTNESS`, key `.bright`.

It calls `apply` with the whole state each time one of them changes while Lighting is not at Default:

```c
typedef struct phx_rgb_state {
    int32_t mode;          /* PHX_RGB_OFF (turn the lights off), PHX_RGB_SOLID, or 2 + index into effects */
    uint8_t r, g, b, reserved;
    int32_t brightness;    /* 0..100 */
} phx_rgb_state;
```

`restore` hands the lights back to the firmware. It is called at once when the user picks Default, and at exit and after a crash elsewhere if the host had applied a state.

### Host functions

| Function | |
|---|---|
| `add_control`, `add_clock`, `add_fan`, `add_rgb` | Register. Only inside `phx_plugin_init`, and only kept if it returns `PHX_OK`. Return `PHX_OK` or `PHX_ERROR`; the reason goes to the log as *plugin <id>: <what> rejected (<why>)*: a bad size, key, type, page or label, a missing required callback, too many, or a duplicate key. |
| `update` | Re-read a control after you changed it. Any thread. |
| `cfg_get_int`, `cfg_set_int`, `cfg_get_str`, `cfg_set_str` | Your own section in `phawx.ini`; see [Your own settings](#your-own-settings). |
| `log` | One line to the log; see [Logging](#logging). `phx_logf(host, self, fmt, ...)` formats it with your own C runtime first. |
| `toast` | A short message: inside the overlay while it is open, otherwise near the bottom of the screen. Copied. |
| `set_status` | A line under your name on the Plugins page (at most 127 characters, copied): why you are not running, or a note while you are (*Running · <text>*). `NULL` clears it. |
| `plugin_dir` | The folder your DLL was loaded from, without a trailing backslash, for example `C:\Program Files\PhawxON\plugins\ryzenadj`. Load your own dependencies by full path from here. |
| `ctl_get` | Any control's value and state by its full key (`"cpu.tdp"`, `"example.fan.mode"`): 0 with the value (`def` at Default) and `active`, or -1 if it does not exist or is hidden. During `phx_plugin_init` saved values are not loaded yet, so you get `def` and 0; AutoTDP's rows, the Settings rows and the rows of plugins with later ids do not exist yet. |
| `hw_ready` | 1 if PawnIO is loaded and running, 0 if not. |
| `ec_read`, `ec_write` | One byte of the ACPI embedded controller (ports 0x62/0x66) through PawnIO's signed LpcACPIEC module, under the `Global\Access_EC` lock that other tools share. 0 or -1. |
| `msr_read`, `msr_write` | A model-specific register through PawnIO's signed modules. Only the registers those modules allow will work. `cpu` -1 means any CPU. 0 or -1. |
| `platform` | CPU vendor, family, model and stepping, core counts, and the SMBIOS maker, product and board strings to match your device against. |

### Your own settings

`cfg_*` read and write your own section, `[plugin:<id>]`, in `phawx.ini`, for settings that are not controls:

- Keys follow the control key rules (1 to 40 letters, digits, `_`, `-`, `.`) and, like all ini keys, ignore case.
- Values are UTF-8 strings of at most 511 characters, without line breaks. Quotes and spaces around a value are stripped when it is read back.
- `cfg_set_int` and `cfg_set_str` return `PHX_OK`, or `PHX_ERROR` when they refuse a bad key, a line break or a value that is too long, or cannot write; nothing is written then, and the log says why. `cfg_set_str(p, key, NULL)` deletes the key.
- `cfg_get_int` returns `def` if the key is missing or its value is not a number (decimal, or hex with `0x`).
- `cfg_get_str` returns 1 if the key exists and 0 (with `""`) if not. A value longer than your buffer is cut after the last whole UTF-8 character that fits; a buffer of 1537 bytes holds any value.

### Logging

`host->log` writes one line: `plugin <id>: <your text>`, cut at 400 bytes. It always goes to `OutputDebugString`, and also to `phawx.log` next to `phawx.ini` when the environment variable `PHAWX_LOG` is set. The text is UTF-8. `phx_logf` formats into a 512-byte buffer first; with MinGW its format string is checked like `printf`'s.

### Threads

"UI thread" is Phawx ON's main thread, which also runs startup and a normal exit.

| Callback | Thread |
|---|---|
| `phx_plugin_init` | UI thread, once, at start. |
| control `get`, `fmt` | UI thread. |
| control `set` | UI thread: when the user changes the row, and when saved values are applied again. |
| clock `set_max`, `reset` | AutoTDP's worker while AutoTDP runs; the UI thread for the CPU/GPU max clock rows, a GPU domain switch and when AutoTDP stops; the exit thread. |
| clock `cur` | UI thread, every 0.5 s while the overlay is open and AutoTDP is off. |
| clock `util` | AutoTDP's worker, every 250 ms while a game runs. |
| fan and RGB callbacks | UI thread, and the exit thread. |
| `tick`, `resume` | UI thread. `tick` runs about once a second while the overlay is open, and also whenever saved values are applied again, with the overlay closed. |
| `shutdown` | The exit thread. Also on the UI thread right after init if the host has no room for your rows. |

The exit thread is the UI thread on a normal exit, logoff or restart, and the crash handler's own thread after a crash elsewhere in the process. Do not rely on thread-local storage in `shutdown`.

- **One at a time.** The host never runs two of your callbacks at the same time (one lock per plugin), so you need no lock of your own for state they share. Your own threads still need one.
- **Never nested.** If a callback pumps messages (a message box, an STA COM call), the host does not call you again on that thread until it returns: nested `get`, `fmt`, `tick`, `resume`, clock, fan and light calls are skipped (`fmt` shows no value), and a nested `set()` fails with *Could not apply <label>*. Other threads wait for the lock as usual.
- **Keep it short.** Every callback should return well within 50 ms: the overlay or AutoTDP waits for it.
- **Calling the host.** `log`, `toast`, `set_status`, `update`, `ctl_get`, `plugin_dir`, `hw_ready`, `ec_*` and `msr_*` may be called from any thread, including your own. Call `cfg_*` from inside a callback or `phx_plugin_init`, and `add_*` only inside `phx_plugin_init`.

### Limits

| | |
|---|---|
| Plugins | 16 discovered. |
| Per plugin | 48 controls, 4 clock domains, 4 fans, 4 lights. |
| In total | 1024 rows for Phawx ON and all plugins; 8 CPU and 8 GPU clock domains. |
| Row budget | A plugin needs one row per control, one header per page it uses without a header of its own (on the System page always one), 5 rows per fan and 4 per light. If its rows would leave fewer than 160 free (the Hardware info table, AutoTDP and the Settings page register after the plugins), it is not started: its `shutdown` is called, it shows *too many settings*, and it stays loaded but is never called again. |
| Choices | 64 per choice row, 8 effects per light. |
| Keys | 40 characters (79 with the plugin id), which can all be pinned. |
| Texts | `set_status` 127 characters, log lines 400 bytes, `cfg` values 511 characters, `PhawxWarning` 319 characters. |

### What the host keeps and what it copies

- **Kept until the process exits:** every `phx_control`, `phx_clock`, `phx_fan` and `phx_rgb` you add, and their `label`, `unit`, `choices` (the array and its strings), `name`, `effects` and `desc`. Use static storage. The host writes `value` and `active` into your controls, so they must not be `const`.
- **Copied:** `key` and `id`; your `phx_info` and its `name` and `version` (right after `phx_plugin_init` returns, so point `*info` at a static one); the strings passed to `set_status`, `toast`, `log` and `cfg_*`.
- **The host's:** `host`, `host->platform` and its strings, `self` and the string `plugin_dir` returns belong to Phawx ON and stay valid for the whole process.

Phawx ON never unloads a running plugin, so function pointers stay valid too.

### Versions and struct sizes

Every struct starts with its size. Set it to `sizeof` of the struct as your header declares it. The host accepts any size of at least the version 1 size (`PHX_CONTROL_SIZE_V1` 120, `PHX_CLOCK_SIZE_V1` 80, `PHX_FAN_SIZE_V1` 56, `PHX_RGB_SIZE_V1` 56) and never reads or writes past the size you give. `phx_info` may even be smaller (the host reads the whole members that fit). So:

- a plugin built with a newer header, with bigger structs, still works on this Phawx ON as long as it only uses version 1 features;
- a newer Phawx ON only appends members, so version 1 plugins keep working. Check `PHX_HOST_HAS(host, member)` before using a host function added later, and `host->version` for the API version as a whole.

`phx_info.version` is not shown anywhere yet.

## Loading your own DLLs

Phawx ON runs as administrator, so it restricts where Windows looks for DLLs, for the whole process, plugins included:

- A plugin's DLL is loaded with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, so the DLLs it **links to** are found in its own folder and in System32, and their own dependencies in their folders.
- A DLL loaded **by bare name** at run time (`LoadLibrary("foo.dll")`), by you or by a library you use, is looked for only in the Phawx ON folder, in System32 and in folders added with `AddDllDirectory`, never in `PATH`, the current directory or your plugin's folder.

So:

- Load your own DLLs by full path, with the same flags:
  ```c
  wchar_t path[MAX_PATH];
  _snwprintf(path, MAX_PATH, L"%s\\vendor.dll", host->plugin_dir(self));
  path[MAX_PATH - 1] = 0;
  HMODULE h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  ```
- If a vendor library loads another DLL by bare name, load that DLL yourself by full path first: a DLL that is already loaded is found by its name. The RyzenAdj plugin does this with `inpoutx64.dll`.
- Do not delay-load your own DLLs, and do not call `SetDllDirectory`, `SetDefaultDllDirectories` or `AddDllDirectory`: they change the search for all of Phawx ON.
- A 64-bit plugin cannot load 32-bit DLLs.

## Rules

- **Undo everything in `shutdown`.** It runs when Phawx ON exits, on logoff and restart, and after a crash elsewhere, possibly on another thread. Phawx ON's promise is that its changes do not outlive it, and that includes yours. Remember that returning a `PHX_F_OPTIONAL` row to Default does not call you.
- **Start at Default.** Use `PHX_F_OPTIONAL` for anything that changes hardware, so nothing changes until the user asks.
- **Make `set()` idempotent.** It is called again with the same value after sleep, on AC/DC changes and on profile switches.
- **Go through PawnIO or the vendor's own API.** Do not bring an unsigned or known-vulnerable kernel driver of your own. `ec_*` and `msr_*` cover most handheld fan and LED controllers. The RyzenAdj plugin is an exception for machines without PawnIO, and says so. If yours has to be one too, say what it brings in `PhawxWarning`.
- **Keep what you register valid until the process exits.** Structs, strings, choice lists: use static storage.
- **Do not let C++ exceptions or `longjmp` cross into Phawx ON**, and never free memory the other side allocated.
- **Keep callbacks fast**, and put anything that can block on your own thread with a timeout. A hang after startup is not caught.
- **Check `host->version` against what you need**, not against `PHX_API_VERSION`, and return `PHX_E_VERSION` if it is older.

## The RyzenAdj plugin

`plugins\ryzenadj\ryzenadj.dll` ships with Phawx ON and is off by default. It controls AMD APU power limits through [RyzenAdj](https://github.com/FlyGoat/RyzenAdj)'s `libryzenadj.dll`, for machines where Phawx ON's own SMU control (through PawnIO) does not work.

### Driver risk

libryzenadj does not use PawnIO. It reaches the hardware through two old kernel drivers that let **any program** on the PC use them, without administrator rights:

- **WinRing0** (`WinRing0x64.sys`, CVE-2020-14979). It is loaded while the plugin runs: during that time any program on this PC can use it to read and write physical memory, MSRs and I/O ports, which means taking full control of the PC. The plugin removes it when Phawx ON exits, unless the plugin itself crashed or stopped responding.
- **InpOut** (`inpoutx64.sys`), used only with *Read power table*. Its DLL installs the driver as a Windows service that starts with Windows and **stays installed after Phawx ON exits**, so the same exposure remains, even with Phawx ON closed, until you remove it (see below).

The question asked before turning the plugin on says so too: *Uses the WinRing0 driver while it runs. Read power table also installs the InpOut driver, which stays installed and loads at every boot. Any program on this PC can use these drivers to take full control of it.* Microsoft's vulnerable driver blocklist and Memory Integrity block these drivers on many systems; Phawx ON never turns either off to make this work. If the driver is blocked, the plugin shows *RyzenAdj could not open its driver (Windows may block WinRing0)* and stays off. Prefer PawnIO where it works.

### Setup

1. Copy `libryzenadj.dll`, `WinRing0x64.dll` and `WinRing0x64.sys` from RyzenAdj's Windows release into `plugins\ryzenadj\`. Copy `inpoutx64.dll` as well only if you want *Read power table*; it is never loaded unless that option is on, and never on CPUs without a readable power table.
2. Turn the plugin on and restart Phawx ON.

It does not start when it is not needed or cannot work:

| Status | Why |
|---|---|
| *needs an AMD Ryzen APU* | Not an AMD CPU. |
| *not needed, Phawx ON controls this APU itself* | Phawx ON's own AMD TDP control works, so the two never fight over the same limit. |
| *RyzenAdj does not support this CPU* | The CPU is not on libryzenadj 0.19's list (checked from CPUID before any DLL is loaded). |
| *libryzenadj.dll is missing from plugins\ryzenadj* | Step 1. |
| *libryzenadj.dll or a DLL it needs could not be loaded* | Usually `WinRing0x64.dll` is missing. |
| *this libryzenadj.dll is too old or not RyzenAdj* | An entry point it needs is missing. |
| *another tool is holding the SMU* | `Global\Access_PCI` was not free within 500 ms. |
| *RyzenAdj could not open its driver (Windows may block WinRing0)* | See the driver risk above. |
| *RyzenAdj stopped responding* | Startup did not finish within 5 seconds. |
| *could not start its worker thread*, *could not register its controls* | Rare. |

### Controls

On the CPU page, under **RyzenAdj**:

- **APU power** and **Temperature** readouts. They show *Needs Read power table* while that option is off, and are hidden when it is on but the table cannot be read, and on CPUs without a readable table.
- **TDP (sustained)**, **Boost TDP (fast)**, **Slow limit** (advanced) and **Temperature limit** (60–105 °C). The power sliders start at 3 W and go up to the value Phawx ON's own AMD control uses for the CPU, from its CPUID family and model: 30 W on Van Gogh and Mendocino, 140 W on Strix Halo, 180 W on Dragon Range and Fire Range, 65 W on the others, raised to the firmware's own limit if that is higher. Boost is never set below TDP, and Slow follows TDP unless you set it.
- **Read power table**, a toggle, off by default and not offered on every CPU (see below).
- On Raven, Picasso, Dali and Lucienne it also gives AutoTDP an iGPU clock domain, *iGPU (RyzenAdj)*.

The sliders are `PHX_F_OPTIONAL`: choosing Default, or holding X or R to reset one, does not change the hardware. The limit you set last stays until Phawx ON exits, and is put back then only when *Read power table* is on and the table was readable.

### Read power table

libryzenadj cannot read the power table of Van Gogh (family 17h, models 90h and 91h), Mendocino (17h A0h), Dragon Range (19h 61h) or Fire Range (1Ah 44h). On those CPUs the option and the two readouts are hidden, InpOut is never loaded, and the status says *no power table on this CPU, limits are not restored*.

With the option off (the default), or on those CPUs, libryzenadj can set the limits but not read them. The sliders start at Default without the firmware's values, there are no power or temperature readings, and the limits Phawx ON found are not put back on exit. Because the firmware's boost limit is unknown, every TDP change also sets Boost to the same value while Boost is at Default (on Van Gogh, which ships with 15 W for all three, TDP above 15 W would otherwise have no effect). Lowering TDP then lowers Boost too.

Turning it on asks first, with the text below, and takes effect at the next start (*Restart Phawx ON to apply*). It is stored as `table=1` in `[plugin:ryzenadj]`, not in profiles. At the next start the plugin loads `plugins\ryzenadj\inpoutx64.dll` by full path, and libryzenadj reads the firmware's power table through it. Then the sliders start at the firmware's values, the readouts work, and the original limits are put back on exit. Turning it off again also applies at the next start, and says *Restart to apply. InpOut stays installed*: turning it off does not remove the driver.

> *Reads the power table through the InpOut driver. Its DLL installs the driver as a Windows service (inpoutx64) that stays after Phawx ON exits. To remove it: sc delete inpoutx64, restart, then delete C:\Windows\System32\drivers\inpoutx64.sys. Takes effect after a restart.*

The InpOut DLL copies `inpoutx64.sys` to `C:\Windows\System32\drivers\` and creates an auto-start kernel service named `inpoutx64`. It stays after Phawx ON exits and loads at every boot. To remove it:

1. Turn *Read power table* (or the whole plugin) off and restart Phawx ON, so the driver is not installed again.
2. In an administrator command prompt run `sc delete inpoutx64`.
3. Restart Windows.
4. Delete `C:\Windows\System32\drivers\inpoutx64.sys`.

### What is restored, and when

At exit (normal exit, logoff, restart, and after a crash elsewhere in Phawx ON) the plugin:

- puts back the limits it changed to the values it read at start, **only when *Read power table* is on and the table was readable**. Otherwise the limits you set stay until the firmware changes them or the PC restarts;
- removes its iGPU clock cap (Phawx ON also resets that domain itself if AutoTDP or the GPU max clock row capped it);
- closes libryzenadj (or, without the power table, only WinRing0, because libryzenadj 0.17 and later crash in their cleanup when the table was never read).

Nothing is restored if the plugin itself crashed, or after *RyzenAdj stopped responding*.

### Status while running

| Status | Meaning |
|---|---|
| *Running · limits not read back or restored on exit* | Read power table is off. |
| *Running · no power table on this CPU, limits are not restored* | Van Gogh, Mendocino, Dragon Range or Fire Range: the option is not offered. |
| *Running · InpOut driver stays installed after exit* | Read power table is on and working. |
| *Running · no inpoutx64.dll, limits are not restored* | It is on, but `inpoutx64.dll` is missing or could not be loaded. |
| *Running · no power table, limits are not restored* | It is on, but the table could not be read. |
| *Running · RyzenAdj stopped responding* | A libryzenadj call did not return within 1 second. From then on every RyzenAdj control fails (*Could not apply …*), nothing is restored at exit, and exit is not delayed. |

### Timeouts and locking

libryzenadj waits for the SMU's answer without a timeout, so the plugin runs every libryzenadj call on a worker thread of its own. The UI and AutoTDP wait at most 1 second per call, and startup at most 5 seconds; a call that does not come back makes the plugin give up on the library (*RyzenAdj stopped responding*, and the log line *libryzenadj did not return within <ms> ms, no longer calling it*).

On that thread the plugin takes `Global\Access_PCI` around every call, waiting up to 500 ms, like Phawx ON's own SMU code and other tools that share the SMU. libryzenadj itself does not take that lock, so other RyzenAdj-based tools running at the same time are **not** kept from talking to the SMU at the same moment. Do not run them alongside.

At start the log shows the CPU family and model, libryzenadj's own family number, the slider range and whether the power table is readable.

## Troubleshooting

### The plugin is not listed

| Cause | Fix |
|---|---|
| It is not in `plugins\` or one folder below it, or it sits in a symbolic link or junction. | Move the file itself there. |
| The export is missing or decorated. | Export `phx_plugin_init` with exactly that name. Include `phawx_plugin.h` in the file that defines it, so C++ gets C linkage. Check with `x86_64-w64-mingw32-objdump -p mydevice.dll` or `dumpbin /exports mydevice.dll`. |
| Another plugin has the same id (for example `MyDevice.dll` and `mydevice.v2.dll`). | Rename one; see [The plugin id](#the-plugin-id). The log names the one it skipped. |
| 16 plugins were found already. | Remove plugins you do not use. |
| The file is over 64 MB, or not a DLL. | |

### Listed, but not running

| Status | Fix |
|---|---|
| *Not running* | It is off. Turn it on and restart. |
| *Not running · starts after a restart* | Use **Restart Phawx ON to apply**. |
| *Not running · install Phawx ON in Program Files* | Move the Phawx ON folder to `C:\Program Files\`. |
| *Not running · built for 32-bit Windows, needs a 64-bit build* | Build for x64. |
| *Not running · not built for x64 Windows, needs an x64 build* | It is a 64-bit DLL for another CPU (such as ARM64). Build for x64. |
| *Not running · its file name is reserved, rename it* | See the reserved ids under [The plugin id](#the-plugin-id). |
| *Not running · a DLL it needs is missing* | Something the plugin links to is not in its folder or in System32: `libstdc++-6.dll` for a MinGW C++ build (link with `-static`), `vcruntime140.dll` for an MSVC `/MD` build, or a vendor DLL (put the plugin and the DLL in their own folder in `plugins\`). |
| *Not running · could not be loaded (error n)* | Windows could not load the DLL; the number is the Windows error code. |
| *Not running · not a Phawx ON plugin* | The file names `phx_plugin_init` in its export table, but Windows found no such export. |
| *Not running · not for this device* | `phx_plugin_init` returned `PHX_UNSUPPORTED`. |
| *Not running · needs a newer Phawx ON* | It returned `PHX_E_VERSION`. |
| *Not running · failed to start (n)* | It returned `n` without calling `set_status`. |
| *Not running · too many settings* | Its rows do not fit; see [Limits](#limits). |
| *Not running · turned off after it crashed Phawx ON* | Fix the crash, then turn it back on. Run with `PHAWX_LOG=1` to see how far it got; the log names the address of the crash. |
| *Not running · <other text>* | The plugin's own `set_status` reason. |

### Running, but rows are missing

Check `phawx.log` for *rejected*: a bad size, key or label, a missing `set()`, a choice row without choices, a slider with `max < min`, a duplicate key, or a registration made after `phx_plugin_init` returned. Also check that the row is not `PHX_F_HIDDEN`, and that it is not `PHX_F_ADVANCED` with advanced options off. *registry: no room for …* means Phawx ON ran out of rows.
