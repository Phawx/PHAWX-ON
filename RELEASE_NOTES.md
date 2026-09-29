Phawx ON 1.4

New
- **Core sliders on the Quick page.** How many cores Windows may keep unparked, as a count: **P-cores** and **E-cores** on hybrid CPUs, **Cores** on CPUs with one kind of core ("4 of 6"). They replace the *max unparked* percentages on the CPU page; a saved global value is carried over. AutoTDP never goes above what you set here.
- **Watch AutoTDP work.** While it runs, the sliders it manages (CPU and GPU max clock, EPP, cores) show the value it has set right now and glide as it changes them. The one it is trying turns amber and reads *Trying …*. Settings it is not managing stay adjustable: the cores when *Find the fewest cores* is off, EPP when it is left to Windows.
- **UI scale** in Settings: make the overlay smaller or larger, 75–200 %.

Phawx ON 1.3

New
- **AutoTDP holds the frame rate with the least power.** It still lowers the CPU and GPU clocks while the frame rate holds. It now also looks for the fewest cores Windows needs to keep unparked (per core type on hybrid CPUs) and for the highest EPP that still holds the frame rate. Each change is checked against the power draw: on battery without a charger, the battery's discharge rate for the whole machine; otherwise the CPU package (Intel RAPL, AMD power table) plus an NVIDIA GPU. It tries one thing at a time. A clock step near the limit is kept unless the draw goes up. Fewer cores or a higher EPP is kept only when the machine then draws clearly less at the same frame rate and 1% lows, and the clocks may rise a little to make up for it. When frames fall short it goes back toward all cores and EPP 0 at once. At least two cores stay unparked, and on hybrid CPUs two P-cores and an E-core. It never allows more cores than your own *Max unparked cores* settings do. After a pause or alt-tab of under a minute it picks up where it was.
- **Power use** on the Quick page, under the AutoTDP status: the draw, the unparked cores, the EPP and what AutoTDP is trying. The header strip shows the draw too.
- **Battery draw** in Hardware info: what the machine takes from the battery.
- `make sim` runs AutoTDP against a simulated handheld and checks the frame rate, the power against the best any setting could reach, the core floors, and that everything is restored.

Changed
- **Find the fewest cores** (formerly *Dynamic core parking*, CPU → AutoTDP tuning) is on by default and works on all CPUs with three cores or more, not only hybrid ones. It parks cores step by step instead of half of a core type at once.
- **EPP while active** (formerly *Force EPP 0 while active*) has three settings. *Tune for power* is the new default. *0 (fastest)* is the old behaviour. *Windows* leaves EPP alone. A saved On or Off keeps its meaning (0 or Windows).
- Without a game, AutoTDP now puts core parking and a tuned EPP back to Windows' settings, instead of leaving them as they were.
- When the target cannot be reached, AutoTDP holds the frame rate it does reach, a little under it, and lowers what is not the limit (such as the CPU clock in a GPU-bound game). Before, it kept everything at full speed.

Phawx ON 1.2

New
- **System page sections.** The Power tab is now the Power section of the System page, followed by Fan control, Lighting, Windows, a section for each plugin, and Hardware info.
- **Fan curves.** Fan mode has a new **Curve** setting for the GPD Win Mini fan and for plugin fans. Draw the fan speed against the CPU temperature on a graph (7 points, 30–90 °C) by dragging the points, or pick a point with left/right and press A to change it with up/down. Phawx ON follows the curve about once a second, always runs the fan at 100 % from 95 °C, and hands the fan back to the firmware if it cannot read the temperature.
- **Lighting section.** RGB lights from plugins are grouped in their own section.
- **Hardware info.** A table at the end of the System page lists everything Phawx ON can read: system, BIOS and Windows, CPU name, cores, load, clock, power and temperature, memory, GPU readings, display mode, fan speeds, power source and battery, and the state of PawnIO and PresentMon.

Changed
- AMD APUs without a power table (Van Gogh, Mendocino and others) now show their temperature, read from the SMN thermal register, on the CPU page and for fan curves.
- Plugins: rows on `PHX_PAGE_POWER` are shown on the System page. On System each plugin gets a section of its own.

Phawx ON 1.1.1

Fixed in 1.1.1
- **PawnIO showed "modules are missing" after installing PawnIO.** PawnIO's installer only installs the driver; programs are meant to bring the modules they use. Phawx ON now ships the signed modules it needs (IntelMSR, IntelMCHBAR, AMDFamily17, RyzenSMU, LpcACPIEC from PawnIO.Modules 0.2.11) in `plugins\PawnIO\modules\`, with a copy built into the exe, so installing PawnIO from pawnio.eu is all that is needed. To update the modules later, copy newer ones from the PawnIO.Modules releases into that folder; its README explains how.
- **AMD: TDP above the firmware's boost limit had no effect** on APUs without a readable power table (Van Gogh, Mendocino, Dragon Range, Fire Range). Boost at Default now follows TDP there.

Phawx ON 1.1

New
- **Pins.** Settings have a pin. Tap it, or press Y on the gamepad, to add that setting to the Quick page. Pinned settings appear below the built-in Quick controls and are saved in `phawx.ini`.
- **Hold to reset.** Hold X on the gamepad or R on the keyboard for half a second to set the selected setting back to Default. On a touch screen, press and hold the row for 2 seconds (half a second with a mouse). A bar along the row fills while you hold. This only works on saved settings; on actions and links a long press is a normal tap, and moving your finger cancels it.
- **Plugins page.** The top shows whether PawnIO and PresentMon are installed and running, with their versions, or what is wrong. Each missing one gets a Download link to its official site. Below that, every plugin in the `plugins` folder is listed with an on/off switch and its status.
- **Plugins.** DLLs dropped into `plugins\` next to `PhawxON.exe` can add settings, AutoTDP clock domains, fans and RGB lights. They are off until you turn them on (Phawx ON asks first), changes apply after a restart, and they load only when Phawx ON is installed under Program Files. A plugin that crashes or hangs Phawx ON is turned off on the next start, and the hardware is still restored. The guide is `sdk\PLUGINS.md` in the zip, with the SDK header and an example plugin.
- **RyzenAdj plugin** (included, off by default). Place `libryzenadj.dll` and WinRing0 from RyzenAdj's Windows release in `plugins\ryzenadj\` to control AMD APUs that the built-in backend does not cover. It stays off on APUs Phawx ON already handles itself. **Know the risk before you turn it on:** while it runs, any program on this PC can use the WinRing0 driver to take full control of it. Its opt-in *Read power table* setting also installs the InpOut driver, which has the same problem and stays installed after Phawx ON exits until you remove it (`sc delete inpoutx64`, restart, delete `C:\Windows\System32\drivers\inpoutx64.sys`). Without that setting the limits it sets are not restored on exit. Details in `sdk\PLUGINS.md`.

Changed
- Per-game profiles are now off by default. "Save profile for current game" (Settings) turns them on. If your 1.0 config already has game profiles, they stay on.
- Y on the gamepad now pins and unpins settings. AutoTDP is still on the Quick page switch and in the tray menu.
- Links open in your normal (non-administrator) browser. If that is not possible, the link is copied to the clipboard.
- Security: Phawx ON no longer loads DLLs from `PATH` or the current directory, and no longer trusts `%ProgramFiles%` (which a user can redirect) when it looks for PawnIO, PresentMon or NVIDIA's libraries.
- PhawxON.exe no longer stores about 100 KB of zero-filled data. With the PawnIO modules built in (1.1.1) it is about 310 KB.

Fixed
- The crash handler now runs. In 1.0 it never did, so a crash could leave TDP and clock limits in place until reboot. After a crash they are now restored, also when the crash is in a plugin.

Install: extract the zip to `C:\Program Files\`, so the app is in `C:\Program Files\PhawxON\`. Requirements: [PawnIO](https://pawnio.eu) for hardware control (its modules come with Phawx ON), and [Intel PresentMon](https://game.intel.com/story/intel-presentmon/) for AutoTDP. See the README for details.

Not yet tested on real hardware. Please report issues.
