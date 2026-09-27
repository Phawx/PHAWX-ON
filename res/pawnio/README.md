# PawnIO modules

Signed PawnIO modules that Phawx ON builds into `PhawxON.exe` as resources
(`res/phawx.rc`). The PawnIO installer only installs the driver and
`PawnIOLib.dll`; programs are expected to ship the modules they use.

- Source: [namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules), release 0.2.11,
  `release_0_2_11.zip` (SHA-256 `43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4`)
- License: LGPL-2.1, see `COPYING` in this folder
- Files, unmodified:

| File | Used for | SHA-256 |
|---|---|---|
| `IntelMSR.bin` | Intel MSRs (power limits, RAPL) | `d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f` |
| `IntelMCHBAR.bin` | Intel MCHBAR power limit mirror (read-only) | `3f82b832d99b4aac37d2a20fdb7c9baa2a3bc0488612c9019c9484eb0e8a6eae` |
| `AMDFamily17.bin` | AMD Zen MSRs (CPPC, boost) | `dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91` |
| `RyzenSMU.bin` | AMD SMU mailbox and PM table | `301d9ca397108e09f31bfbd5ac4c9bb4f352a5de68532c32db3ba7ddcde93450` |
| `LpcACPIEC.bin` | ACPI embedded controller (fans) | `c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906` |

To update: download the new release zip, replace these five files, update the
table above and rebuild. A module file with the same name in PawnIO's install
folder (or its `modules` folder), or next to `PhawxON.exe` (or in its `modules`
or `PawnIO` folder), is used only when PawnIO does not accept the built-in one.
