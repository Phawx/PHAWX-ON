Phawx ON 1.1

New
- **Pins.** Every setting row has a pin. Tap it, or press Y on the gamepad, to add that setting to the Quick page. Pinned settings appear below the built-in Quick controls and are saved in `phawx.ini`.
- **Hold to reset.** Hold X on the gamepad (the button in the X position on other pads) or R on the keyboard for half a second to set the selected setting back to its default. On a touch screen, press and hold the row for 2 seconds. A bar along the row fills while you hold.
- **Plugins page.** The top shows whether PawnIO and PresentMon are installed and running, with their versions. Each missing one gets a Download link to its official site. Below that, every plugin in the `plugins` folder is listed with an on/off switch and its status.
- **Plugins.** C DLLs dropped into `plugins\` next to `PhawxON.exe` can add settings, AutoTDP clock domains, fans and RGB lights. They are off until you turn them on, and they load only when Phawx ON is installed under Program Files. A plugin that crashes or hangs Phawx ON is turned off on the next start. See `sdk\PLUGINS.md` in the zip for how to write one.
- **RyzenAdj plugin** (included). Place `libryzenadj.dll` and its driver files in `plugins\ryzenadj\` to control AMD APUs that the built-in backend does not cover. It stays off on APUs Phawx ON already handles itself.

Changed
- Per-game profiles are now off by default. "Save profile for current game" (Settings) turns them on. If your 1.0 config already has game profiles, they stay on.
- Y on the gamepad now pins and unpins settings. AutoTDP is still on the Quick page switch and in the tray menu.
- Links open in your normal (non-administrator) browser. If that is not possible, the link is copied to the clipboard.

Fixed
- The crash handler now runs. In 1.0 it never did, so a crash could leave TDP and clock limits in place until reboot. After a crash they are now restored.

Requirements: [PawnIO](https://pawnio.eu) with its signed modules for hardware control, and [Intel PresentMon](https://game.intel.com/story/intel-presentmon/) for AutoTDP. Install to Program Files for Start with Windows and plugins. See the README for details.

Not yet tested on real hardware. Please report issues.
