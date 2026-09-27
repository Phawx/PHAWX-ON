# AMD APU SMU control (native)

Scope: talking to the AMD System Management Unit (MP1 firmware, "PSMU/RSMU" mailbox) from a Win32 C app. Our kernel driver provides PCI config read/write on bus 0 dev 0 fn 0 and physical memory mapping. This is a re-implementation spec built from facts (addresses, message IDs, units). No code is copied from LGPL/GPL sources.

Sources read (commit/state as of 2026-09):
- RyzenAdj `lib/nb_smu_ops.{c,h}`, `lib/api.c`, `lib/cpuid.c`, `lib/ryzenadj.h`, `lib/win32/osdep_win32.cpp`, `main.c` (github.com/FlyGoat/RyzenAdj, HEAD 5775fc3, 2026-05-13, v0.19.0)
- ryzen_smu Linux driver `smu.c`, `drv.c` (github.com/amkillam/ryzen_smu, fork of leogx9r)
- G-Helper `app/Pawn/RyzenSmu.cs` (github.com/seerge/g-helper), uses PawnIO
- Universal x86 Tuning Utility `Scripts/AMD Backend/RyzenSmu.cs`, `Scripts/Family.cs`, `Views/Pages/CustomPresets.xaml.cs` (github.com/JamesCJ60/Universal-x86-Tuning-Utility)
- ZenStates-Core `Hardware/PowerTable.cs` (github.com/irusanov/ZenStates-Core)
- steam-deck-tools `PowerControl/Helpers/AMD/VangoghGPU.cs` (github.com/ayufan/steam-deck-tools)
- Linux amdgpu `drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v11_5_ppsmc.h`, `smu_v12_0_ppsmc.h`, `smu_v13_0_1_ppsmc.h`, `smu_v13_0_4_ppsmc.h`, `smu_v13_0_5_ppsmc.h`, `smu_v14_0_0_ppsmc.h`, `smu13/smu_v13_0_4_ppt.c`, `smu14/smu_v14_0_0_ppt.c`

---

## 1. SMN access (System Management Network) via host bridge 0:0.0

The SMN is AMD's internal register bus. The root complex at PCI 0:0.0 has an index/data register pair in config space:

| PCI cfg offset (0:0.0) | Name | Use |
|---|---|---|
| `0xB8` | `NB_SMN_INDEX` | write the 32-bit SMN address |
| `0xBC` | `NB_SMN_DATA`  | then read or write the 32-bit value |

```c
#define NB_SMN_INDEX 0xB8
#define NB_SMN_DATA  0xBC
/* drv_pci_rd32 / drv_pci_wr32 are our driver IOCTLs for bus0/dev0/fn0 config dwords */
static uint32_t smn_rd(uint32_t a){ drv_pci_wr32(NB_SMN_INDEX, a & ~3u); return drv_pci_rd32(NB_SMN_DATA); }
static void     smn_wr(uint32_t a, uint32_t v){ drv_pci_wr32(NB_SMN_INDEX, a); drv_pci_wr32(NB_SMN_DATA, v); }
```

Facts and pitfalls:
- The index/data pair is **not atomic**. Every other tool using it (HWiNFO, Ryzen Master, LibreHardwareMonitor, ZenTimings, G-Helper/UXTU/HandheldCompanion, AMD drivers) can interleave with you. Serialize **both** the index/data pair and the whole mailbox transaction. Convention on Windows: take the named mutex `Global\Access_PCI` (used by LHM/HWiNFO family of tools) around each index+data pair, and hold a process-local lock around a whole mailbox transaction. The mailbox itself has no ownership, so a second tool writing msg/arg registers mid-transaction corrupts yours. There is no fix for that. Keep transactions short and check responses.
- The PCI address is encoded as `bus<<8 | dev<<3 | fn`. RyzenAdj memsets it to 0, which means 0:0.0.
- RyzenAdj masks `addr & ~3` on read only. All mailbox addresses are dword-aligned anyway.
- ryzen_smu attaches to root complex PCI device IDs (vendor 0x1022): 0x1450, 0x15D0, 0x1480, 0x1630, 0x14B5 (17h MA0h / 19h M40h), 0x14A4, 0x14D8, 0x14E8, 0x153A, 0x1507, 0x1122, plus MI200/MI300. The Windows side does not need to match on these. Always use 0:0.0, and optionally sanity-check vendor 0x1022 at cfg offset 0.
- Secure/locked platforms: RyzenAdj's probe writes 0x47 into the ARG0 register and reads it back. If that fails, the PCI path is blocked ("check secure boot"). Do the same probe.
- Driver: PawnIO's signed RyzenSMU / AMDFamily17 modules provide the SMN and SMU access.

## 2. SMU mailbox protocol

Each mailbox has three SMN registers: **MSG** (message ID), **RSP** (response/status) and **ARG base** (6 consecutive dwords ARG0..ARG5 at `base + 4*i`).

Response codes (identical across all SMU generations, also in amdgpu `PPSMC_Result_*`):

```c
enum { SMU_RSP_BUSY_OR_PENDING = 0x00, SMU_OK = 0x01, SMU_FAILED = 0xFF,
       SMU_UNKNOWN_CMD = 0xFE, SMU_REJECTED_PREREQ = 0xFD, SMU_REJECTED_BUSY = 0xFC };
```

Transaction (RyzenAdj order, with ryzen_smu's pre-wait and G-Helper's wall-clock timeout added):
1. (Recommended, from ryzen_smu and G-Helper) wait until `RSP != 0`, which means the previous command has finished. If it stays 0 past the timeout, return BUSY and do not write.
2. `RSP = 0`
3. `ARG[0..5] = args` (write all 6, unused = 0)
4. `MSG = id`. This triggers execution.
5. Poll `RSP` until non-zero, with a timeout. RyzenAdj **spins forever with no timeout**. Do not copy that. ryzen_smu uses 8192 attempts by default. G-Helper uses 200 ms wall clock and a spin → `SwitchToThread` → `Sleep(1)` ladder.
6. If `RSP == 1`, read back `ARG[0..5]`. Getters return their data in ARG0 (and ARG1 for 64-bit values).

```c
typedef struct { uint32_t msg, rsp, arg; } smu_mbox;
static int smu_send(const smu_mbox *m, uint32_t id, uint32_t a[6]) {
    uint64_t t0 = now_us(); uint32_t r;
    while (!(r = smn_rd(m->rsp))) if (now_us()-t0 > 200000) return -SMU_REJECTED_BUSY;
    smn_wr(m->rsp, 0);
    for (int i = 0; i < 6; i++) smn_wr(m->arg + 4*i, a[i]);
    smn_wr(m->msg, id);
    t0 = now_us(); unsigned spin = 0;
    while (!(r = smn_rd(m->rsp))) {
        if (now_us()-t0 > 200000) return -1;           /* timeout */
        if (++spin > 256) Sleep(1); else if (spin > 32) SwitchToThread();
    }
    if (r != SMU_OK) return -(int)r;
    for (int i = 0; i < 6; i++) a[i] = smn_rd(m->arg + 4*i);
    return 0;
}
```

Test and version messages (same ID on every generation):
- `0x01` TestMessage. RyzenAdj sends it at init on **both** MP1 and PSMU and expects RSP == 1. Use it to validate the address set for the detected family.
- `0x02` GetSmuVersion (MP1). ARG0 = version. ryzen_smu sends arg0=1. Decode as `(v>>16)&0xFF . (v>>8)&0xFF . v&0xFF` (top byte = program ID on some parts).
- `0x03` GetDriverIfVersion / "BIOS interface version" (MP1, RyzenAdj `get_bios_if_ver`).

RyzenAdj's error mapping: `0xFE` → "unsupported" (message absent on this firmware), `0xFD`/`0xFC`/`0xFF` → "rejected". Treat 0xFE as "feature not present, stop trying". Treat 0xFD on a PM-table transfer as retryable (see §6).

## 3. Mailbox addresses per family

| Family group | MP1 MSG | MP1 RSP | MP1 ARG | PSMU MSG | PSMU RSP | PSMU ARG |
|---|---|---|---|---|---|---|
| Raven, Picasso, Dali, Renoir, Lucienne, Cezanne ("MP1 set 1") | `0x03B10528` | `0x03B10564` | `0x03B10998` | `0x03B10A20` | `0x03B10A80` | `0x03B10A88` |
| Van Gogh, Rembrandt, Mendocino, Phoenix, Hawk Point ("set 2") | `0x03B10528` | `0x03B10578` | `0x03B10998` | `0x03B10A20` | `0x03B10A80` | `0x03B10A88` |
| Strix Point, Krackan Point, Strix Halo ("set 3") | `0x03B10928` | `0x03B10978` | `0x03B10998` | `0x03B10A20` | `0x03B10A80` | `0x03B10A88` |
| Dragon Range, Fire Range (and desktop Matisse/Vermeer/Raphael/Granite Ridge) ("set 4") | `0x03B10530` | `0x03B1057C` | `0x03B109C4` | `0x03B10524` | `0x03B10570` | `0x03B10A40` |
| Zen1/Zen+ desktop (Summit/Pinnacle; not needed) | `0x03B10528` | `0x03B10564` | `0x03B10598` | `0x03B1051C` | `0x03B10568` | `0x03B10590` |
| Shimada Peak (G-Helper only) | n/a | n/a | n/a | `0x03B10924` | `0x03B10970` | `0x03B10A40` |

Notes:
- RyzenAdj, ryzen_smu, G-Helper and UXTU all agree on the table above.
- **Van Gogh PSMU is uncertain.** ryzen_smu explicitly disables RSMU on Van Gogh ("not supported or unknown"). RyzenAdj still probes PSMU at `0x03B10A20` and fails init if the TestMessage fails, yet it is widely used on Steam Deck under Windows, so the probe apparently succeeds there. Make PSMU optional: if TestMessage on PSMU fails, keep going with MP1 only.
- HSMP mailbox (desktop/server only): MSG `0x03B10534`, RSP `0x03B10980`, ARG `0x03B109E0`. Not needed.
- The **amdgpu driver mailbox** is a separate MP1 mailbox (`C2PMSG_66/82/90`). See §8. Do not confuse it with the MP1 mailbox above.

```c
typedef struct { smu_mbox mp1, psmu; } smu_addrs;
static const smu_addrs SMU_SET1 = {{0x03B10528,0x03B10564,0x03B10998},{0x03B10A20,0x03B10A80,0x03B10A88}};
static const smu_addrs SMU_SET2 = {{0x03B10528,0x03B10578,0x03B10998},{0x03B10A20,0x03B10A80,0x03B10A88}};
static const smu_addrs SMU_SET3 = {{0x03B10928,0x03B10978,0x03B10998},{0x03B10A20,0x03B10A80,0x03B10A88}};
static const smu_addrs SMU_SET4 = {{0x03B10530,0x03B1057C,0x03B109C4},{0x03B10524,0x03B10570,0x03B10A40}};
```

## 4. CPU family detection (CPUID)

`CPUID(0).EBX,EDX,ECX` must be "AuthenticAMD". From `CPUID(1).EAX`:
`family = ((eax>>8)&0xF) + ((eax>>20)&0xFF)`; `model = ((eax>>4)&0xF) | ((eax>>12)&0xF0)`.

```c
typedef enum {
  AF_UNKNOWN=-1, AF_RAVEN=0, AF_PICASSO, AF_DALI, AF_RENOIR, AF_LUCIENNE, AF_CEZANNE,
  AF_VANGOGH, AF_REMBRANDT, AF_MENDOCINO, AF_PHOENIX, AF_HAWKPOINT,
  AF_KRACKAN, AF_STRIX, AF_STRIXHALO, AF_DRAGON, AF_FIRE, AF_COUNT
} amd_fam;

typedef struct { uint8_t fam, model; int8_t af; const smu_addrs *mb; } amd_cpu_id;
static const amd_cpu_id AMD_IDS[] = {
  {0x17, 0x11, AF_RAVEN,     &SMU_SET1}, {0x17, 0x12, AF_RAVEN, &SMU_SET1}, /* 0x12: UXTU only */
  {0x17, 0x18, AF_PICASSO,   &SMU_SET1},
  {0x17, 0x20, AF_DALI,      &SMU_SET1},   /* also Pollock (UXTU splits by name) */
  {0x17, 0x60, AF_RENOIR,    &SMU_SET1},
  {0x17, 0x68, AF_LUCIENNE,  &SMU_SET1},
  {0x17, 0x90, AF_VANGOGH,   &SMU_SET2}, {0x17, 0x91, AF_VANGOGH, &SMU_SET2}, /* 0x91 = Sephiroth (OLED Deck) */
  {0x17, 0xA0, AF_MENDOCINO, &SMU_SET2},
  {0x19, 0x50, AF_CEZANNE,   &SMU_SET1},   /* Cezanne/Barcelo */
  {0x19, 0x40, AF_REMBRANDT, &SMU_SET2}, {0x19, 0x44, AF_REMBRANDT, &SMU_SET2}, /* 0x3F: UXTU also */
  {0x19, 0x61, AF_DRAGON,    &SMU_SET4},   /* same model as Raphael desktop */
  {0x19, 0x74, AF_PHOENIX,   &SMU_SET2}, {0x19, 0x78, AF_PHOENIX, &SMU_SET2}, /* 0x78 = Phoenix2 */
  {0x19, 0x75, AF_HAWKPOINT, &SMU_SET2}, {0x19, 0x7C, AF_HAWKPOINT, &SMU_SET2}, /* 0x7C: UXTU HawkPoint2 */
  {0x1A, 0x20, AF_STRIX,     &SMU_SET3}, {0x1A, 0x24, AF_STRIX, &SMU_SET3},
  {0x1A, 0x44, AF_FIRE,      &SMU_SET4},   /* same model as Granite Ridge desktop */
  {0x1A, 0x60, AF_KRACKAN,   &SMU_SET3}, {0x1A, 0x68, AF_KRACKAN, &SMU_SET3}, /* 0x68: UXTU Krackan2 */
  {0x1A, 0x70, AF_STRIXHALO, &SMU_SET3},
};
```

Pitfalls:
- RyzenAdj's list covers only the "official" models (decimal in source: 17,24,32,96,104,144,145,160 / 80,64,68,97,116,120,117 / 32,36,68,96,112). The extra rows (0x12, 0x3F, 0x7C, 0x68 on 1Ah) come from UXTU and are unverified.
- ryzen_smu labels 1Ah model 0x60 "Strix Point (Ryzen AI 350)". That chip is Krackan. The SMU address set is the same, so it does not matter.
- Dragon Range vs Raphael and Fire Range vs Granite Ridge share CPUID models. The SMU interface is also shared, so no split is needed for SMU use. UXTU splits by "HX" in the brand string only to choose a label.
- Unknown model inside a known family → fall back to "nearest" set by family: 17h → SET1, 19h → SET2, 1Ah → SET3. Validate with TestMessage on MP1 before sending anything else.

## 5. Settings → message IDs

Units of ARG0 (RyzenAdj CLI semantics):
- Power limits (stapm/fast/slow/apu-slow/skin-temp-limit): **mW**.
- Currents (vrm*, psi*): **mA**.
- Times (stapm-time, slow-time): **seconds**.
- `tctl-temp`: **°C integer**.
- `apu-skin-temp`, `dgpu-skin-temp`: **°C × 256** (Q8.8). RyzenAdj multiplies by 256 internally.
- Clock limits and gfx-clk / oc-clk: **MHz**.
- `oc-volt`: VID code = `(1.55 − V) / 0.00625` (SVI2 VID, Renoir-class only).
- `power-saving` / `max-performance` / `enable-oc` / `disable-oc`: arg = 0. The first two are the SMU side of ALIB's "DC/AC" profile hints. Effect is OEM/firmware dependent.
- `prochot-deassertion-ramp`: raw value, where higher = tighter limits after PROCHOT ends.

Curve Optimizer encodings:
- `set-coall` / `set-cogfx`: 20-bit two's complement: `arg = (uint32_t)value & 0xFFFFF` (negative −N → `0x100000 − N`, e.g. −20 → `0xFFFEC`). Positive values are passed as-is.
- `set-coper`: `arg = (sel << 20) | ((uint32_t)value & 0xFFFFF)`, where UXTU builds `sel = ((ccd << 4 | ccx) << 4) | (core & 0xF)`, i.e. bits 20-23 = core, 24-27 = CCX, 28-31 = CCD. On single-CCD APUs, `sel = core`. G-Helper masks the value with `0xFFFF` instead of `0xFFFFF` (bits 16-19 then differ for negative offsets). **Uncertain which the firmware wants. Prefer the 20-bit form (UXTU, matches coall).** Zen4c/Zen5c core numbering on Phoenix2/Strix/Krackan (whether compact cores sit in a second CCX index) is **unverified**.
- CO values are in "counts" (roughly 3–5 mV each). Typical safe range is −30..0. Settings are **volatile** (lost on reboot and on some S3/S0ix resumes).

### 5.1 C table (RyzenAdj-verified)

`mb`: 0 = MP1, 1 = PSMU. Multiple rows for the same setting and family are tried **in order** until one returns OK. RyzenAdj does this for stapm (MP1 then PSMU) and for the oc-* messages.

```c
enum { MB_MP1 = 0, MB_PSMU = 1 };
typedef enum {
  S_STAPM_LIMIT, S_FAST_LIMIT, S_SLOW_LIMIT, S_SLOW_TIME, S_STAPM_TIME, S_TCTL_TEMP,
  S_VRM_CUR, S_VRMSOC_CUR, S_VRMGFX_CUR, S_VRMCVIP_CUR, S_VRMMAX_CUR, S_VRMGFXMAX_CUR, S_VRMSOCMAX_CUR,
  S_PSI0_CUR, S_PSI0SOC_CUR, S_PSI3CPU_CUR, S_PSI3GFX_CUR,
  S_MAX_GFXCLK, S_MIN_GFXCLK, S_MAX_SOCCLK, S_MIN_SOCCLK, S_MAX_FCLK, S_MIN_FCLK,
  S_MAX_VCN, S_MIN_VCN, S_MAX_LCLK, S_MIN_LCLK,
  S_PROCHOT_RAMP, S_APU_SKIN_TEMP, S_DGPU_SKIN_TEMP, S_APU_SLOW_LIMIT, S_SKIN_TEMP_PWR,
  S_GFX_CLK, S_POWER_SAVING, S_MAX_PERF,
  S_OC_CLK, S_PER_CORE_OC_CLK, S_OC_VOLT, S_ENABLE_OC, S_DISABLE_OC,
  S_CO_ALL, S_CO_PER, S_CO_GFX, S_COUNT
} smu_setting;

#define B(f) (1u << (f))
#define G_RV  (B(AF_RAVEN)|B(AF_PICASSO)|B(AF_DALI))
#define G_RN  (B(AF_RENOIR)|B(AF_LUCIENNE)|B(AF_CEZANNE))
#define G_VG  (B(AF_VANGOGH))
#define G_RMB (B(AF_REMBRANDT))
#define G_MDN (B(AF_MENDOCINO))
#define G_PHX (B(AF_PHOENIX)|B(AF_HAWKPOINT))
#define G_STX (B(AF_KRACKAN)|B(AF_STRIX)|B(AF_STRIXHALO))
#define G_DR  (B(AF_DRAGON)|B(AF_FIRE))
#define G_MOB (G_RN|G_VG|G_RMB|G_MDN|G_PHX|G_STX)   /* "Renoir and later APUs" */

typedef struct { uint8_t s, mb; uint16_t msg; uint32_t fams; } smu_cmd;
static const smu_cmd SMU_CMDS[] = {
  {S_STAPM_LIMIT, MB_MP1, 0x1A, G_RV}, {S_STAPM_LIMIT, MB_MP1, 0x14, G_MOB},
  {S_STAPM_LIMIT, MB_PSMU,0x31, G_MOB}, {S_STAPM_LIMIT, MB_MP1, 0x4F, G_DR},
  {S_FAST_LIMIT,  MB_MP1, 0x1B, G_RV}, {S_FAST_LIMIT,  MB_MP1, 0x15, G_MOB}, {S_FAST_LIMIT, MB_MP1, 0x3E, G_DR},
  {S_SLOW_LIMIT,  MB_MP1, 0x1C, G_RV}, {S_SLOW_LIMIT,  MB_MP1, 0x16, G_MOB}, {S_SLOW_LIMIT, MB_MP1, 0x5F, G_DR},
  {S_SLOW_TIME,   MB_MP1, 0x1D, G_RV}, {S_SLOW_TIME,   MB_MP1, 0x17, G_MOB}, {S_SLOW_TIME,  MB_MP1, 0x60, G_DR},
  {S_STAPM_TIME,  MB_MP1, 0x1E, G_RV}, {S_STAPM_TIME,  MB_MP1, 0x18, G_MOB}, {S_STAPM_TIME, MB_MP1, 0x4E, G_DR},
  {S_TCTL_TEMP,   MB_MP1, 0x1F, G_RV}, {S_TCTL_TEMP,   MB_MP1, 0x19, G_MOB}, {S_TCTL_TEMP,  MB_MP1, 0x3F, G_DR},
  {S_VRM_CUR,     MB_MP1, 0x20, G_RV}, {S_VRM_CUR,     MB_MP1, 0x1A, G_MOB}, {S_VRM_CUR,    MB_MP1, 0x3C, G_DR},
  {S_VRMSOC_CUR,  MB_MP1, 0x21, G_RV}, {S_VRMSOC_CUR,  MB_MP1, 0x1B, G_MOB},
  {S_VRMGFX_CUR,  MB_MP1, 0x1C, G_VG},
  {S_VRMCVIP_CUR, MB_MP1, 0x1D, G_VG},
  {S_VRMMAX_CUR,  MB_MP1, 0x22, G_RV}, {S_VRMMAX_CUR,  MB_MP1, 0x1C, G_RN|G_RMB|G_MDN|G_PHX|G_STX},
  {S_VRMMAX_CUR,  MB_MP1, 0x1E, G_VG},
  {S_VRMGFXMAX_CUR,MB_MP1,0x1F, G_VG},
  {S_VRMSOCMAX_CUR,MB_MP1,0x23, G_RV}, {S_VRMSOCMAX_CUR,MB_MP1,0x1D, G_RN|G_RMB|G_MDN|G_PHX|G_STX},
  {S_PSI0_CUR,    MB_MP1, 0x24, G_RV}, {S_PSI0_CUR,    MB_MP1, 0x1E, G_RN},
  {S_PSI0SOC_CUR, MB_MP1, 0x25, G_RV}, {S_PSI0SOC_CUR, MB_MP1, 0x1F, G_RN},
  {S_PSI3CPU_CUR, MB_MP1, 0x20, G_VG},
  {S_PSI3GFX_CUR, MB_MP1, 0x21, G_VG},
  {S_MAX_GFXCLK,  MB_MP1, 0x46, G_RV|B(AF_LUCIENNE)},
  {S_MIN_GFXCLK,  MB_MP1, 0x47, G_RV|B(AF_LUCIENNE)},
  {S_MAX_SOCCLK,  MB_MP1, 0x48, G_RV}, {S_MIN_SOCCLK, MB_MP1, 0x49, G_RV},
  {S_MAX_FCLK,    MB_MP1, 0x4A, G_RV}, {S_MIN_FCLK,   MB_MP1, 0x4B, G_RV},
  {S_MAX_VCN,     MB_MP1, 0x4C, G_RV}, {S_MIN_VCN,    MB_MP1, 0x4D, G_RV},
  {S_MAX_LCLK,    MB_MP1, 0x4E, G_RV}, {S_MIN_LCLK,   MB_MP1, 0x4F, G_RV},
  {S_PROCHOT_RAMP,MB_MP1, 0x26, G_RV}, {S_PROCHOT_RAMP,MB_MP1, 0x20, G_RN},
  {S_PROCHOT_RAMP,MB_MP1, 0x22, G_VG}, {S_PROCHOT_RAMP,MB_MP1, 0x1F, G_RMB|G_MDN|G_PHX|G_STX},
  {S_APU_SKIN_TEMP, MB_MP1,0x38, G_RN}, {S_APU_SKIN_TEMP, MB_MP1,0x33, G_VG|G_RMB|G_MDN|G_PHX},
  {S_DGPU_SKIN_TEMP,MB_MP1,0x39, G_RN}, {S_DGPU_SKIN_TEMP,MB_MP1,0x34, G_VG|G_RMB|G_MDN|G_PHX|B(AF_STRIX)},
  {S_APU_SLOW_LIMIT,MB_MP1,0x21, G_RN}, {S_APU_SLOW_LIMIT,MB_MP1,0x23, G_RMB|G_PHX|G_STX},
  {S_SKIN_TEMP_PWR, MB_MP1,0x53, G_RN}, {S_SKIN_TEMP_PWR, MB_MP1,0x4A, G_VG|G_RMB|G_MDN|G_PHX|G_STX},
  {S_GFX_CLK,     MB_PSMU,0x89, G_MOB},
  {S_POWER_SAVING,MB_MP1, 0x19, G_RV}, {S_POWER_SAVING,MB_MP1, 0x12, G_MOB},
  {S_MAX_PERF,    MB_MP1, 0x18, G_RV}, {S_MAX_PERF,    MB_MP1, 0x11, G_MOB},
  {S_OC_CLK,      MB_MP1, 0x31, G_RN|G_RMB}, {S_OC_CLK,      MB_PSMU,0x19, G_RN|G_RMB},
  {S_PER_CORE_OC_CLK,MB_MP1,0x32,G_RN|G_RMB}, {S_PER_CORE_OC_CLK,MB_PSMU,0x1A,G_RN|G_RMB},
  {S_OC_VOLT,     MB_MP1, 0x33, G_RN}, {S_OC_VOLT,     MB_PSMU,0x1B, G_RN},
  {S_ENABLE_OC,   MB_MP1, 0x2F, G_RN}, {S_ENABLE_OC,   MB_PSMU,0x17, G_RMB},
  {S_DISABLE_OC,  MB_MP1, 0x30, G_RN}, {S_DISABLE_OC,  MB_PSMU,0x1D, G_RN}, {S_DISABLE_OC, MB_PSMU,0x18, G_RMB},
  {S_CO_ALL,      MB_MP1, 0x55, G_RN}, {S_CO_ALL,      MB_MP1, 0x4C, G_VG|G_RMB|G_PHX|G_STX},
  {S_CO_ALL,      MB_PSMU,0x07, G_DR},
  {S_CO_PER,      MB_MP1, 0x54, G_RN}, {S_CO_PER,      MB_MP1, 0x4B, G_VG|G_RMB|G_PHX|G_STX},
  {S_CO_PER,      MB_PSMU,0x06, G_DR},
  {S_CO_GFX,      MB_MP1, 0x64, G_RN}, {S_CO_GFX,      MB_PSMU,0xB7, G_VG|G_RMB|G_PHX},
};
```

Gaps per family (RyzenAdj returns FAM_UNSUPPORTED):
- **Dragon/Fire Range**: no vrmsoc, vrmmax, vrmsocmax, psi*, skin temps, apu-slow, skin-temp-limit, gfx-clk, power-saving, max-performance, oc-*, cogfx.
- **Van Gogh**: no vrmsocmax, apu-slow-limit, psi0/psi0soc (it has psi3cpu/psi3gfx instead), oc-*. It has the extra GFX/CVIP rails. **No max/min gfxclk via this mailbox**; use the driver mailbox (§8).
- **Mendocino**: no CO (all/per/gfx), no apu-slow.
- **Strix Point/Krackan**: no apu-skin-temp and no cogfx in RyzenAdj. Strix Halo: no apu/dgpu skin temp ("controlled only via tctl-temp"), and PSMU 0xB7 cogfx is **rejected**.
- **Max/min gfxclk, socclk, fclk, vcn, lclk**: only Raven/Picasso/Dali (gfxclk also Lucienne) via the ryzenadj mailboxes. On every other APU there is **no RyzenAdj way to cap GPU clock** except `gfx-clk` (PSMU 0x89, forces a fixed clock; help text says "Renoir only", comment says added for testing on KRK/STX/STXH). See §8 for the real per-APU clock caps.
- `oc-clk` on Rembrandt+ forces a **fixed all-core CCLK** (disables boost). Useful as a hard CPU clock cap for AutoTDP only if the firmware accepts it (Renoir/Cezanne/Rembrandt confirmed in RyzenAdj; other gens unknown). `disable-oc` restores normal boost.

RyzenAdj bugs to not replicate:
- `set_stapm_time` has a **missing `break`**: on the mobile families it sends MP1 0x18 and then also 0x4E (the Dragon Range ID). Send only 0x18.
- `set_vrmsoc_current`, `set_vrmgfx_current` etc. also lack `break`s, but they fall into `default` harmlessly.
- 0x3F0000 fclk offset in RyzenAdj is `0x3C5`, which is misaligned and certainly a typo (probably `0x3C0`). Mem clk is at `0x3C4`.

### 5.2 Extra messages seen only in UXTU (unverified, try-and-check)

UXTU sends each command to **all** listed mailboxes in parallel (MP1 = true, PSMU = false). IDs not in RyzenAdj:

| Setting | Raven/Picasso/Dali | Renoir/Lucienne/Cezanne | Van Gogh | Rembrandt→Strix Halo (FT6/FP7/FP8) | Dragon/Fire Range (AM5) |
|---|---|---|---|---|---|
| enable-feature / disable-feature | MP1 0x05 / 0x06 | MP1 0x05 / 0x07 | MP1 0x05 / 0x07 | MP1 0x05 / 0x07 | MP1 0x03 / 0x04 |
| stapm-limit (PSMU) | 0x2E | 0x31 | 0x31 | 0x31 | – |
| fast / slow / slow-time / stapm-time (PSMU) | 0x30/0x2F/0x31/0x32 | 0x32/0x33/0x35/0x36 | – | 0x32/0x33/0x35/0x36 | slow PSMU 0xCB; stapm-time **MP1 0x53** (RyzenAdj says 0x4E) |
| tctl (PSMU) / cHTC-temp | 0x33 / PSMU 0x56 | – / PSMU 0x37 | PSMU 0x37 | MP1 0x63, PSMU 0x37 | PSMU 0x59 |
| max/min cpuclk | MP1 0x44 / 0x45 | – | – | – | – |
| max/min gfxclk (PSMU) | 0x68 / 0x69 | – | – | – | – |
| gfx-clk alt | – | PSMU 0x1C | PSMU 0x1C | PSMU 0x1C | – |
| pbo-scalar | MP1 0x57, PSMU 0x63 | MP1 0x49, PSMU 0x3F | – | PSMU 0x3E | PSMU 0x5B |
| set-coall (PSMU) | 0x59 | 0xB1 | 0x5D | 0x5D | 0x07 (+MP1 0x36) |
| set-coper (PSMU) | 0x58 | 0x52 | – | 0x53 | 0x06 (+MP1 0x35) |
| set-cogfx | PSMU 0x59 | PSMU 0x53 | PSMU 0xB7 | PSMU 0xB7 | PSMU 0xA7 |
| apu-skin / dgpu-skin (PSMU) | – | 0x91 / 0x92 | – | 0x91 / 0x92 | – |
| get-sustained-power-and-thm-limit | MP1 0x43 | MP1 0x5B | MP1 0x54 | MP1 0x5F | MP1 0x23 |
| fused limits (PSMU, getters) | 0x7F-0x84 | 0x11-0x16 | – | 0x11-0x16, tctl 0xE5 | 0xD9-0xDE |

G-Helper additionally uses **PSMU 0x5D** as a fallback CO-all on Strix Halo, and sends Renoir stapm/fast/slow to both MP1 and PSMU (0x31/0x32/0x33+0x34).

## 6. PM table (telemetry)

The SMU writes a float32 array (the "PM table", IEEE-754 little-endian, 4-byte stride) into a firmware-reserved DRAM region on request. Our driver must map that physical address (read-only).

### 6.1 Messages (all on **PSMU**)

| Family | Get table version | Get DRAM address | Transfer table → DRAM |
|---|---|---|---|
| Raven/Picasso/Dali | `0x0C` → ARG0 | `0x0B`, arg0 = 3 → ARG0 (32-bit) | `0x3D`, arg0 = 3 |
| Renoir/Lucienne/Cezanne | `0x06` → ARG0 | `0x66` → ARG0 (32-bit) | `0x65` |
| Rembrandt/Phoenix/HawkPoint/Krackan/Strix/StrixHalo | `0x06` → ARG0 | `0x66` → **ARG1:ARG0 (64-bit)** | `0x65` |
| Dragon/Fire Range (ryzen_smu "Raphael/GraniteRidge", not in RyzenAdj) | `0x05` | `0x04`, arg0 = arg1 = 1 → ARG1:ARG0 | `0x03` |
| Van Gogh, Mendocino | **not supported** by RyzenAdj or ryzen_smu | | |

Notes:
- ryzen_smu sends 0x66 with arg0 = arg1 = 1 and transfer 0x65 with arg0 = 3 on Renoir+/Rembrandt+ (arg0 = 0 on Cezanne). RyzenAdj sends all zeros. Both reportedly work. Use RyzenAdj's zeros first, and try ryzen_smu's args if RSP ≠ 1.
- Raven/Picasso actually have **two** tables (ryzen_smu): primary 0x608 bytes plus secondary 0xA4 bytes (transfer 0x3D arg0 = 5, DRAM address via 0x0A/0x3D/0x0B dance). RyzenAdj reads only the primary. Ignore the secondary.
- The DRAM address is **constant after boot** (ryzen_smu caches it). Fetch once, map once.
- **Transfer throttling**: the SMU rejects (0xFD) transfers issued too fast. This applies across all tools. RyzenAdj's strategy:
  1. Before transferring, compare the first 6 floats (24 bytes) in the mapped memory with your last copy. If they changed, another tool just refreshed and you can skip your transfer.
  2. On 0xFD, `Sleep(10)` and retry, then `Sleep(100)` and retry.
  3. On Raven/Picasso the first transfer after boot returns OK with an all-zero table: sleep 10 ms and transfer again.
  ryzen_smu enforces ≥ 1 ms between transfers. G-Helper transfers twice with a 100 ms gap after resume (the first call returns BUSY right after S3/S0ix resume).
- For AutoTDP, poll at ≥ 100 ms (the SMU averages internally anyway). 250–500 ms is plenty for power/temps. Do not use the PM table as the fast FPS-loop signal.

### 6.2 Mapping on Windows

RyzenAdj on Windows uses **inpoutx64.dll** `MapPhysToLin(phys, 0x1000, &handle)` (maps 4 KiB only), then `memcpy`. Phawx ON reads the PM table through PawnIO's signed RyzenSMU module instead of mapping physical memory itself.

**Table sizes disagree between sources.** Never trust a single size. Map at least 0x2000 bytes (enough for every known table, ≤ 0x1028), and clamp reads to the size you know.

### 6.3 Table versions and sizes

| Version | Family | Size (RyzenAdj) | Size (others) |
|---|---|---|---|
| 0x1E0001 / 02 / 03 | Raven/Picasso | 0x568 / 0x580 / 0x578 | ZSC 0x570/0x570/0x608 |
| 0x1E0004, 05, 0A, 0101 | Raven/Picasso/Dali | 0x608 | |
| 0x370000 / 01 / 02 / 03,04 / 05 | Renoir/Lucienne | 0x794 / 0x884 / 0x88C / 0x8AC / 0x8C8 | ryzen_smu same. ZSC 0x884/0x88C/0x8AC/0x8C8/0x8C0 (shifted by one, likely wrong) |
| 0x3F0000 | labelled "Van Gogh" in getters (table never fetched on VGH by RyzenAdj) | 0x7AC | |
| 0x400001 / 02 / 03 / 04,05 | Cezanne | 0x910 / 0x928 / 0x94C / 0x944 | |
| 0x450004 / 05 | Rembrandt | 0xAA4 / 0xAB0 | |
| 0x4C0003 / 04 / 05 / 06 / 07 / 08 / 09 | Phoenix / Hawk Point | 0xB18 / 0xB1C / 0xAF8 / 0xAFC / 0xB00 / 0xAF0 / 0xB00 | same in ryzen_smu and ZSC |
| 0x5D0008 / 09 / 0B (ZSC also 0A) | Strix Point | 0xD54 | ZSC 0xD54 / 0xD58 / 0xD60 / 0xD60 |
| 0x650004 / 05 / 06 / 07 | Krackan Point | default 0x1000 | ryzen_smu 0x650007 = 0xD54. ZSC 0xB74 / 0xB78 / 0xB80 / 0xB80 |
| 0x64020C (0x64010C same layout) | Strix Halo | 0xE50 | ZSC: 0x640107-0C 0xDC0..0xDDC, 0x640207-0C 0x100C..0x1028 |
| 0x54xxxx / 0x62xxxx | Dragon Range / Fire Range | not in RyzenAdj | ryzen_smu: 0x540000..0x540208 = 0x828..0x8D0, 0x620105 = 0x724, 0x620205 = 0x994 |

### 6.4 Offsets (bytes; value = `((float*)tbl)[off/4]`)

Universal for all APU tables: `0x00` STAPM limit (W), `0x04` STAPM value, `0x08` fast (PPT) limit, `0x0C` fast value, `0x10` slow limit, `0x14` slow value.

Units: power W, current A, temperature °C, gfx/fclk/memclk MHz, core clock **GHz** (per-core arrays; disabled cores read 0), voltage V, time constants s.

```c
typedef enum {
  PM_APU_SLOW_LIM, PM_APU_SLOW_VAL, PM_VRM_LIM, PM_VRM_VAL, PM_VRMSOC_LIM, PM_VRMSOC_VAL,
  PM_VRMMAX_LIM, PM_VRMMAX_VAL, PM_VRMSOCMAX_LIM, PM_VRMSOCMAX_VAL,
  PM_TCTL_LIM, PM_TCTL_VAL, PM_APU_SKIN_LIM, PM_APU_SKIN_VAL, PM_DGPU_SKIN_LIM, PM_DGPU_SKIN_VAL,
  PM_PSI0, PM_PSI0SOC, PM_CCLK_SETPOINT, PM_CCLK_BUSY, PM_SOCKET_PWR, PM_SOC_VOLT, PM_SOC_PWR,
  PM_GFX_CLK, PM_GFX_VOLT, PM_GFX_TEMP, PM_FCLK, PM_MEMCLK, PM_L3_CLK,
  PM_CORE_PWR0, PM_CORE_VOLT0, PM_CORE_TEMP0, PM_CORE_CLK0,   /* base of float[ncores] arrays */
  PM_STAPM_TIME, PM_SLOW_TIME, PM_FCOUNT
} pm_field;
typedef struct { uint32_t ver_lo, ver_hi; uint16_t size; int16_t off[PM_FCOUNT]; uint8_t ncores; } pm_layout;
/* -1 = not known. Fill per row below. */
```

| Field | Raven 0x1E00xx | Renoir 0x37000[0-4] | Renoir 0x370005 | "VGH" 0x3F0000 | Cezanne 0x400004/5 | Rembrandt 0x45000x | Phoenix/HPT 0x4C000x | Strix 0x5D000x | Krackan 0x650005 | Strix Halo 0x64020C |
|---|---|---|---|---|---|---|---|---|---|---|
| apu_slow lim/val | – | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/0x1C | 0x18/– |
| vrm (TDC) lim/val | 0x18/0x1C | 0x20/0x24 | 0x20/0x24 | – | 0x20/0x24 | 0x20/0x24 | 0x20/0x24 | 0x30/0x34 | 0x30/0x34 | – |
| vrmsoc lim/val | 0x20/0x24 | 0x28/0x2C | 0x28/0x2C | – | 0x28/0x2C | 0x28/0x2C | 0x28/0x2C | 0x38/0x3C | 0x38/0x3C | – |
| vrmmax (EDC) lim/val | 0x28/0x2C | 0x30/0x34 | 0x30/0x34 | – | 0x30/0x34 | 0x30/0x34 | 0x30/0x34 | (=vrm) | – | – |
| vrmsocmax lim/val | 0x34/0x38 | 0x38/0x3C | 0x38/0x3C | – | 0x38/0x3C | 0x38/0x3C | 0x38/0x3C | (=vrmsoc) | – | – |
| tctl lim/val | 0x58/0x5C | 0x40/0x44 | 0x40/0x44 | 0x40/0x44 | 0x40/0x44 | 0x40/0x44 | 0x40/0x44 | 0x40/0x44 (*) | 0x40/0x44 | 0x58/0x5C |
| apu skin lim/val | – | 0x58/0x5C | 0x58/0x5C | 0x58/0x5C | 0x58/0x5C | 0x58/0x5C | 0x58/0x5C | 0x58/0x5C | – | 0x58/0x5C |
| dgpu skin lim/val | – | 0x60/0x64 | 0x60/0x64 | – | 0x60/0x64 | 0x60/0x64 | 0x60/0x64 | 0x68/0x6C | – | 0x60/0x64 |
| psi0 / psi0soc | 0x40/0x48 | 0x78/0x80 | 0x78/0x80 | – | 0x78/0x80 | – | 0x78/0x80 | – | – | – |
| cclk setpoint / busy | 0x98/0x9C | 0xFC/0x100 | 0xFC/0x100 | – | 0x100/0x104 | – | – | 0xD0/0xCC | – | – |
| socket power | – | 0x98 | 0x98 | 0xA8 | 0x98 | – | – | 0xD0 (=RAPL pkg) | – | – |
| soc volt / soc power | – | 0x198/0x1A0 | 0x198/0x1A0 | 0x1A0/0x1A8 | 0x19C/0x1A4 | – | – | – | – | – |
| gfx clk / volt / temp | – | 0x5B4/0x5A8/0x5AC | 0x5D0/0x5C4/0x5C8 | 0x388/0x37C/0x380 | 0x648/0x63C/0x640 | – | **unknown** | 0x4C0/0x4B8/0x4C8 (**) | – | 0x558/0x54C/0x550 |
| fclk / memclk | – | 0x5CC/0x5D4 | 0x5E8/0x5F0 | 0x3C0?/0x3C4 | 0x664/0x66C | ZSC: 0x6B0 / MCLK 0x6B8 (0x450005) | ZSC: 0x174 / 0x194 (03–07), 0x164 / 0x184 (08, 09) | 0x4E0 / 0x4EC (MT/s) | ZSC cand.: 0x4C8 / 0x4D0 | – |
| L3 clk | – | 0x568 | 0x584 | 0x35C | 0x614 | – | – | – | – | – |
| core pwr / volt / temp / clk base | – | 0x300/0x320/0x340/0x3A0 | 0x31C/0x33C/0x35C/0x3BC | 0x238/0x248/0x258/0x288 (4 cores) | 0x320/0x340/0x360/0x3C0 | – | – | 0x9D8/0xA08/0xA38/0xA68 (12) | – | 0xB90/0xBD0/0xC10/0xC50 (16) |
| stapm_time / slow_time | 0x5E0/0x5E4 (04+) | 0x768/0x76C (00), 0x858/0x85C (01), 0x860/0x864 (02), 0x880/0x884 (03, 04) | 0x89C/0x8A0 | – | 0x918/0x91C | – | 0x918/0x91C (06–09) | 0x9BC/0x9C0 | 0x90C/0x910 (?) | – |

Other Cezanne sub-versions: gfx clk 0x60C/0x624/0x644 (0x400001/2/3), gfx volt 0x600/0x618/0x638, gfx temp 0x604/0x61C/0x63C, core pwr base 0x304 (0x400001), stapm_time 0x8E4/0x8FC/0x920. Raven stapm_time: 0x564 (0x1E0002), 0x55C (0x1E0003).

(*) Strix Point temperature values (RyzenAdj comments, tested): 0x44 = Zen5 cluster, 0x4C = Zen5c cluster, 0x54 = gfx, 0x5C = SoC. Limits default 100 °C.
(**) Strix Point gfx block per RyzenAdj comment: 0x4B4 = gfx power, 0x4B8 = volt, 0x4BC = temp, 0x4C0/0x4C4 = gfx clk, 0x4C8 = unknown clk. The code reads temp at 0x4C8, which contradicts its own comment. **Prefer 0x4BC for gfx temp.** Strix Point clocks block: 0x4E0..0x548 = target clocks, 0x550..0x5B4 = sampled clocks (FCLK 0x4E0, UCLK 0x4E4, PHY 0x4E8, MT/s 0x4EC, VCLK 0x4F0, SOCCLK 0x4F8, MPIPU 0x50C, IPU 0x510; sampled = +0x6C). The Strix Point "vrmmax/vrmsocmax" fields are the same as vrm/vrmsoc (0x1C–0x2C are zero).
Strix Halo: tctl and apu-skin both read 0x58/0x5C (RyzenAdj "tested").
Phoenix/Hawk Point: RyzenAdj lists 0x4C0006–09 for most fields (0x4C0003–05 only for the first six floats). ZSC adds VDDCR_SOC at 0x1C8 (03–07) / 0x1B8 (08) / 0x1BC (09) and CLDO_VDDP at 0x768/0x774.
G-Helper uses float index 10 (offset 0x28) for tctl on Dragon/Fire Range (0x54xxxx/0x62xxxx), "empirically confirmed".

**For AutoTDP:** on Phoenix/Hawk Point/Rembrandt, where GFX clock and busy data are missing from the PM table, get GPU clock and utilization from D3DKMT/PDH GPU-engine counters or ADLX instead. The PM table is still good for STAPM/PPT limits and values (current package power ≈ `0x04` STAPM value or `0x0C` fast value) and Tctl (`0x44`).

## 7. Apply-and-persist behaviour

- All SMU limits are volatile. They are reset on reboot, and often by **S3/S0ix resume, AC/DC transitions, OEM services** (Armoury Crate, Legion Space, MyASUS, AMD PMF / "Smart Power" / ALIB STT tables), and by Windows power-mode changes on PMF-enabled systems.
- Robust approach: after applying, keep the intended values. Every 1–3 s (and on `WM_POWERBROADCAST` / `PBT_APMRESUMEAUTOMATIC` / `GUID_ACDC_POWER_SOURCE` notifications) read the PM table `0x00/0x08/0x10`. If they differ from the target by more than 0.5 W, re-send. This costs a transfer every few seconds. Alternatively re-send blindly every N seconds (UXTU/HandheldCompanion style), which costs about 3 mailbox transactions.
- Order: send `fast ≥ slow ≥ stapm`. Firmware clamps to fused min/max (e.g. Steam Deck MIN 3000 / MAX 21000 mW per steam-deck-tools, though the Deck actually accepts up to ~30 W on MP1). Out-of-range values return OK but are clamped. Verify via the PM table.
- `power-saving`/`max-performance`: call once after limits if you want the ALIB AC/DC profile. The effect is OEM specific.

## 8. GPU/CPU clock caps on APUs (driver mailbox, what RyzenAdj lacks)

The amdgpu (Linux) and AMD Windows display driver talk to MP1 through a **different mailbox**, the "driver interface" C2PMSG_66/82/90:

| Register | amdgpu name | MMIO dword offset (MP1_BASE 0x16000 + n) | SMN address (derived: 0x03B10000 + n*4) |
|---|---|---|---|
| MSG | `mmMP1_SMN_C2PMSG_66` | 0x16282 | `0x03B10A08` |
| ARG | `mmMP1_SMN_C2PMSG_82` | 0x16292 | `0x03B10A48` |
| RSP | `mmMP1_SMN_C2PMSG_90` | 0x1629A | `0x03B10A68` |

(Used by smu_v13_0_4 = Phoenix/Hawk Point, smu_v14_0_0 = Strix-class, vangogh, yellow_carp. Mendocino smu_v13_0_5 uses C2PMSG_2/33/34 in a different bank. Only one ARG register is used.) steam-deck-tools reaches it via the GPU's MMIO BAR with index/data at BAR + 0x38/0x3C and the dword offsets above. The SMN addresses in the last column are derived from the MP1 SMN base and are **unverified**. Reaching it through 0xB8/0xBC should work, but test first.

**Danger:** the Windows AMD graphics driver owns this mailbox. Concurrent use can collide with it (no lock is shared), and a hung SMU means a TDR or hard hang. steam-deck-tools does it on Van Gogh in production. Use it only with short transactions, retries, and a user opt-in flag per family.

Message IDs (argument = MHz unless noted):

| Msg | Van Gogh (smu_v11_5) | Renoir (smu_v12_0) | Rembrandt (13_0_1) / Phoenix, Hawk Point (13_0_4) / Strix (14_0_0) | Mendocino (13_0_5) |
|---|---|---|---|---|
| SetSoftMinGfxclk | 0x0C | – | 0x09 | 0x08 |
| SetSoftMaxGfxClk | 0x1F | 0x30 | 0x1B | 0x14 |
| SetHardMinGfxClk | 0x20 | 0x31 | 0x1C | 0x15 |
| SetHardMinSocclkByFreq / SoftMax / SoftMin | 0x17 / 0x21 / 0x29 | 0x21 / 0x32 / – | 0x13 / 0x1D / 0x24 | – |
| SetSoftMinFclk / SoftMaxFclkByFreq / HardMinFclkByFreq | 0x18 / 0x22 / 0x28 | – / 0x33 / 0x3F | 0x14 / 0x1E / 0x23 | – |
| SetSoftMinCclk / SetSoftMaxCclk | 0x40 / 0x41 (arg = `core<<20 \| MHz`, sent once per core) | – | not exposed | – |
| SetFastPPTLimit / SetSlowPPTLimit (mW) | 0x49 / 0x4A | – | – | – |
| GetFastPPTLimit / GetSlowPPTLimit | 0x4B / 0x4C | – | – | – |
| GetGfxclkFrequency / GetFclkFrequency | 0x1B / 0x1C | – | – | – |
| GetSmuVersion / TestMessage | 0x02 / 0x01 | | | |

steam-deck-tools clamps: GFX 200–1900 MHz, CPU 1400–4000 MHz, TDP 3000–21000 mW, and gates on known SMU versions (0x43F3900, 0x43F3C05, 0x43F3E00, 0x063F0F00, 0x063F0E00).

For non-Van-Gogh APUs on Windows, the supported way to cap the iGPU is **ADLX** (`IADLXGPUTuningServices` → `IADLXManualGraphicsTuning1/2` max frequency, where supported on the APU) or Radeon "Chill"/FRTC. The driver mailbox messages above are the low-level fallback.

CPU clock caps on APUs (for AutoTDP "lowest clocks"):
1. The Windows PPM "Maximum processor frequency" (`PROCFREQMAX`/`PROCFREQMAX1`, MHz) and "Maximum processor state" (%) power settings. These work through CPPC `highest_perf` on amd-pstate-style firmware. See the EPP research doc.
2. MSR `0xC0010015` HWCR / P-state MSRs are not writable for boost caps on modern APUs. CPPC request MSR `0xC00102B3` (`CPPC_REQ`: bits 7:0 max_perf, 15:8 min_perf, 23:16 des_perf, 31:24 EPP), used when CPPC MSR mode is enabled (`0xC00102B1` CPPC_ENABLE bit 0). Windows' own driver rewrites it, so the power-setting route is more reliable.
3. `oc-clk` (MP1 0x31 / PSMU 0x19) as a fixed-clock cap on Renoir/Cezanne/Rembrandt.
4. Van Gogh driver-mailbox `SetSoftMaxCclk` (0x41).

## 9. Minimal init sequence

```text
1. CPUID → amd_fam + smu_addrs (table §4). Unknown → stop (do not guess on non-listed family).
2. smn probe: write 0x47 to MP1.arg, read back == 0x47, else "PCI blocked".
3. MP1 TestMessage (0x01) == OK, else abort. PSMU TestMessage: OK → psmu_ok = 1 (optional on Van Gogh).
4. Read MP1 0x02 (SMU version) and 0x03 (if-version) for logs and quirk keys.
5. If PM table is supported for family: PSMU get-version, get-addr (64-bit on Rembrandt+), driver map ≥0x2000 bytes RO,
   transfer, verify float[0] (STAPM limit) is plausible (1..200 W), else retry once after 10 ms.
6. Apply settings via SMU_CMDS (first OK wins; 0xFE on all candidates → mark unsupported, hide in UI).
7. Watchdog: re-verify limits on a timer and on power broadcasts; re-apply on drift.
```

## 10. Uncertainties summary

- Van Gogh PSMU mailbox existence; Van Gogh PM table (0x3F0000 layout in RyzenAdj is never fetched on VGH).
- `set-coper` value mask (20-bit vs 16-bit) and Zen4c/Zen5c core index encoding.
- Krackan Point table sizes and offsets (sources disagree; ZSC offsets are "candidates").
- Phoenix/Hawk Point/Rembrandt gfx clock/temp offsets: not published in any source read.
- Strix Point gfx temp at 0x4BC vs 0x4C8.
- Dragon Range stapm-time: MP1 0x4E (RyzenAdj) vs 0x53 (UXTU).
- The driver mailbox SMN addresses (0x03B10A08/0A48/0A68) are derived, not read from a tool that uses the SMN path.
- UXTU-only message IDs (§5.2) are unverified by a second source.
