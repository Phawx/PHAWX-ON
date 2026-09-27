#include "phawx.h"
#include <cpuid.h>

ph_platform g_plat;

void cpuidex(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    unsigned a, b, c, d;
    __cpuid_count(leaf, sub, a, b, c, d);
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}

static void trim(wchar_t *s)
{
    int n = lstrlenW(s), i = 0;
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) MoveMemory(s, s + i, (n - i + 1) * sizeof(wchar_t));
}

static void detect_cpuid(void)
{
    uint32_t r[4], maxl, maxe;
    char vend[13];
    cpuidex(0, 0, r);
    maxl = r[0];
    CopyMemory(vend, &r[1], 4);
    CopyMemory(vend + 4, &r[3], 4);
    CopyMemory(vend + 8, &r[2], 4);
    vend[12] = 0;
    if (!lstrcmpA(vend, "GenuineIntel")) g_plat.vendor = VENDOR_INTEL;
    else if (!lstrcmpA(vend, "AuthenticAMD") || !lstrcmpA(vend, "HygonGenuine")) g_plat.vendor = VENDOR_AMD;
    else g_plat.vendor = VENDOR_UNKNOWN;

    if (maxl >= 1) {
        cpuidex(1, 0, r);
        uint32_t fam = (r[0] >> 8) & 0xF, mod = (r[0] >> 4) & 0xF;
        g_plat.stepping = r[0] & 0xF;
        g_plat.family = fam == 0xF ? fam + ((r[0] >> 20) & 0xFF) : fam;
        g_plat.model = (fam == 6 || fam == 0xF) ? mod | (((r[0] >> 16) & 0xF) << 4) : mod;
    }
    if (g_plat.vendor == VENDOR_INTEL && maxl >= 6) {
        cpuidex(6, 0, r);
        g_plat.hwp = (r[0] >> 7) & 1;
        g_plat.epp = g_plat.hwp && ((r[0] >> 10) & 1);
    }

    cpuidex(0x80000000, 0, r);
    maxe = r[0];
    if (g_plat.vendor == VENDOR_AMD) {
        if (maxe >= 0x80000008) {
            cpuidex(0x80000008, 0, r);
            g_plat.cppc = (r[1] >> 27) & 1;
        }
        g_plat.epp = g_plat.cppc || g_plat.family >= 0x17;
    }
    if (maxe >= 0x80000004) {
        char brand[49];
        for (uint32_t i = 0; i < 3; i++) {
            cpuidex(0x80000002 + i, 0, r);
            CopyMemory(brand + i * 16, r, 16);
        }
        brand[48] = 0;
        MultiByteToWideChar(CP_ACP, 0, brand, -1, g_plat.cpu_name, PH_ARRAY(g_plat.cpu_name));
        trim(g_plat.cpu_name);
    }
    if (!g_plat.cpu_name[0]) {
        reg_get_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                    L"ProcessorNameString", g_plat.cpu_name, PH_ARRAY(g_plat.cpu_name));
        trim(g_plat.cpu_name);
    }
}

static void detect_bios(void)
{
    static const wchar_t *k = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    reg_get_str(HKEY_LOCAL_MACHINE, k, L"SystemManufacturer", g_plat.maker, PH_ARRAY(g_plat.maker));
    reg_get_str(HKEY_LOCAL_MACHINE, k, L"SystemProductName", g_plat.product, PH_ARRAY(g_plat.product));
    reg_get_str(HKEY_LOCAL_MACHINE, k, L"BaseBoardProduct", g_plat.board, PH_ARRAY(g_plat.board));
    trim(g_plat.maker);
    trim(g_plat.product);
    trim(g_plat.board);
}

static void detect_topology(void)
{
    DWORD len = 0;
    int base[64];
    WORD ng = GetActiveProcessorGroupCount();
    if (ng > 64) ng = 64;
    for (WORD g = 0, acc = 0; g < ng; g++) {
        base[g] = acc;
        acc += (WORD)GetActiveProcessorCount(g);
    }
    g_plat.nlogical = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (g_plat.nlogical > 256) g_plat.nlogical = 256;
    g_plat.n_class0 = g_plat.nlogical;

    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    BYTE *buf = len ? ph_alloc(len) : NULL;
    if (!buf) return;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        ph_free(buf);
        return;
    }
    int core = 0, minc = 255, maxc = 0;
    for (DWORD off = 0; off < len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (!e->Size) break;
        if (e->Relationship == RelationProcessorCore) {
            BYTE ec = e->Processor.Reserved[0];
            for (WORD gi = 0; gi < e->Processor.GroupCount; gi++) {
                GROUP_AFFINITY *ga = &e->Processor.GroupMask[gi];
                if (ga->Group >= ng) continue;
                for (int b = 0; b < 64; b++) {
                    if (!(ga->Mask & ((KAFFINITY)1 << b))) continue;
                    int lp = base[ga->Group] + b;
                    if (lp < 0 || lp >= 256) continue;
                    g_plat.cls[lp] = ec;
                    g_plat.core_of[lp] = (uint8_t)(core < 255 ? core : 255);
                }
            }
            if (ec < minc) minc = ec;
            if (ec > maxc) maxc = ec;
            core++;
        }
        off += e->Size;
    }
    ph_free(buf);
    g_plat.ncores = core;
    g_plat.max_class = core ? maxc : 0;
    g_plat.hybrid = core && maxc != minc;
    g_plat.n_class0 = g_plat.n_class1 = 0;
    for (int i = 0; i < g_plat.nlogical; i++) {
        if (g_plat.hybrid && g_plat.cls[i] == g_plat.max_class) g_plat.n_class1++;
        else g_plat.n_class0++;
    }
}

int platform_on_ac(void)
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return 1;
    return s.ACLineStatus != 0;
}

void platform_detect(void)
{
    ZeroMemory(&g_plat, sizeof g_plat);
    detect_cpuid();
    detect_bios();
    detect_topology();
    g_plat.on_ac = platform_on_ac();
    ph_log("platform: vendor %d fam %02x mod %02x \"%ls\" %ls / %ls / %ls, %d lp %d cores, hybrid %d (P %d, E %d), hwp %d epp %d cppc %d",
           g_plat.vendor, (unsigned)g_plat.family, (unsigned)g_plat.model, g_plat.cpu_name, g_plat.maker,
           g_plat.product, g_plat.board, g_plat.nlogical, g_plat.ncores, g_plat.hybrid, g_plat.n_class1,
           g_plat.n_class0, g_plat.hwp, g_plat.epp, g_plat.cppc);
}
