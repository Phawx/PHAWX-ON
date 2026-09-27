# PawnIO modules

Phawx ON reaches the hardware through the signed PawnIO driver. The driver only
runs signed *modules*, and the PawnIO installer does not include any: each
program brings the modules it uses. These are the ones Phawx ON uses:

| File | Used for |
|---|---|
| `IntelMSR.bin` | Intel CPU power limits and energy counters |
| `IntelMCHBAR.bin` | Intel power limit mirror (read only) |
| `AMDFamily17.bin` | AMD Ryzen CPU registers (CPU clock, boost) |
| `RyzenSMU.bin` | AMD power limits, iGPU clock and the power table |
| `LpcACPIEC.bin` | Embedded controller (fan control) |

Phawx ON loads each module from this folder first. If one is missing here, it
uses the copy built into `PhawxON.exe` (the version below). As a last resort it
looks in PawnIO's own folder (`C:\Program Files\PawnIO\` or its `modules\`).

## Getting the latest modules

1. Open <https://github.com/namazso/PawnIO.Modules/releases> and download the
   `release_x_y_z.zip` of the newest release.
2. Close Phawx ON (tray icon → Exit).
3. From the zip, copy the five files listed above into this folder
   (`plugins\PawnIO\modules\` next to `PhawxON.exe`), replacing the old ones.
   The other files in the zip are not used by Phawx ON and can be left out.
4. Start Phawx ON. The Plugins page should show *PawnIO x.y is loaded and running*.

If PawnIO refuses a module (for example a file that is damaged or not signed),
Phawx ON falls back to its built-in copy. With `PHAWX_LOG=1` set, `phawx.log`
names the file each module was loaded from, or why it was refused.

Only take modules from the official PawnIO.Modules releases: they run inside the
PawnIO driver, and the driver accepts only modules signed by the PawnIO project.
When Phawx ON is installed under `C:\Program Files\`, only administrators can
change this folder.

## Version and license

The files shipped here and built into `PhawxON.exe` are unmodified copies from
PawnIO.Modules **0.2.11** (`release_0_2_11.zip`, SHA-256
`43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4`). They are
licensed under the LGPL-2.1 (see `COPYING` in this folder); the source is at
<https://github.com/namazso/PawnIO.Modules>.

| File | SHA-256 |
|---|---|
| `IntelMSR.bin` | `d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f` |
| `IntelMCHBAR.bin` | `3f82b832d99b4aac37d2a20fdb7c9baa2a3bc0488612c9019c9484eb0e8a6eae` |
| `AMDFamily17.bin` | `dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91` |
| `RyzenSMU.bin` | `301d9ca397108e09f31bfbd5ac4c9bb4f352a5de68532c32db3ba7ddcde93450` |
| `LpcACPIEC.bin` | `c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906` |

For maintainers: to ship newer modules with Phawx ON itself, replace the five
files in `plugins/PawnIO/modules/` in the repository, update the version and the
checksums above, and rebuild. `res/phawx.rc` builds them into the exe, and
`make dist` copies this folder into the zip.
