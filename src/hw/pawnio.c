#include "phawx.h"
#include "drv_priv.h"
#include <string.h>

typedef HRESULT (WINAPI *pio_version_t)(PULONG);
typedef HRESULT (WINAPI *pio_open_t)(PHANDLE);
typedef HRESULT (WINAPI *pio_load_t)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pio_exec_t)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, ULONG64 *, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pio_close_t)(HANDLE);

enum { M_MSR, M_SMU, M_MCHBAR, M_EC, M_N };

static HMODULE       lib;
static pio_version_t p_version;
static pio_open_t    p_open;
static pio_load_t    p_load;
static pio_exec_t    p_exec;
static pio_close_t   p_close;
static HANDLE        mh[M_N];
static int           is_amd;
static uint64_t      mchbar;
static uint64_t      pm_base;
static uint64_t      pm_resolve_ms;
static uint64_t      pm_cache[512];
static uint64_t      pm_cache_ms;
static int           pm_cache_ok;
static _Thread_local uint32_t smn_index;

char pawnio_name[32] = "PawnIO";
char pawnio_ver[16];
char pawnio_missing[64];   /* CPU modules that did not load, "A.bin, B.bin" */
int  pawnio_state = DRV_MISSING;

static void note_missing(const char *m)
{
    size_t n = strlen(pawnio_missing);
    ph_snprintf(pawnio_missing + n, (int)(sizeof pawnio_missing - n), n ? ", %s" : "%s", m);
}

static int exe_dir(wchar_t *d)
{
    DWORD n = GetModuleFileNameW(NULL, d, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *s = d + n;
    while (s > d && s[-1] != L'\\' && s[-1] != L'/') s--;
    *s = 0;
    return 1;
}

static int install_dir(wchar_t *d)
{
    static const wchar_t *keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        L"SOFTWARE\\PawnIO",
    };
    static const wchar_t *vals[] = { L"InstallLocation", L"InstallDir", L"Path" };
    for (int k = 0; k < PH_ARRAY(keys); k++)
        for (int v = 0; v < PH_ARRAY(vals); v++)
            if (reg_get_str(HKEY_LOCAL_MACHINE, keys[k], vals[v], d, MAX_PATH) == 0 && d[0]) {
                size_t n = wcslen(d);
                if (d[n - 1] == L'"') d[--n] = 0;
                if (d[0] == L'"') memmove(d, d + 1, n * sizeof(wchar_t));
                n = wcslen(d);
                if (n && d[n - 1] != L'\\' && n + 1 < MAX_PATH) { d[n] = L'\\'; d[n + 1] = 0; }
                return 1;
            }
    /* not %ProgramFiles%: a user can point that at a folder they can write to */
    if (ph_program_files(d, MAX_PATH - 10)) return 0;
    size_t n = wcslen(d);
    ph_swprintf(d + n, MAX_PATH - (int)n, L"\\PawnIO\\");
    return 1;
}

static int find_file(const wchar_t *name, wchar_t *out)
{
    wchar_t inst[MAX_PATH], exe[MAX_PATH];
    int hi = install_dir(inst), he = exe_dir(exe);
    const wchar_t *base[5] = { hi ? inst : NULL, hi ? inst : NULL, he ? exe : NULL, he ? exe : NULL, he ? exe : NULL };
    static const wchar_t *sub[5] = { L"", L"modules\\", L"modules\\", L"PawnIO\\", L"" };
    for (int i = 0; i < 5; i++) {
        if (!base[i]) continue;
        ph_swprintf(out, MAX_PATH, L"%s%s%s", base[i], sub[i], name);
        DWORD a = GetFileAttributesW(out);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    return 0;
}

static int exec(int m, const char *fn, const uint64_t *in, int nin, uint64_t *out, int nout)
{
    if (!mh[m] || !p_exec) return -1;
    SIZE_T ret = 0;
    HRESULT hr = p_exec(mh[m], fn, (const ULONG64 *)in, (SIZE_T)nin, (ULONG64 *)out, (SIZE_T)nout, &ret);
    return SUCCEEDED(hr) ? 0 : -1;
}

static void load_module(int m, const wchar_t *file)
{
    wchar_t path[MAX_PATH];
    if (!find_file(file, path)) { ph_log("pawnio: %ls not found", file); return; }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    UCHAR *blob = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (4 << 20) &&
        (blob = ph_alloc((size_t)sz.QuadPart)) != NULL &&
        ReadFile(f, blob, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
        HANDLE h = NULL;
        if (SUCCEEDED(p_open(&h)) && h) {
            if (SUCCEEDED(p_load(h, blob, got))) mh[m] = h;
            else { p_close(h); ph_log("pawnio: %ls rejected", file); }
        }
    }
    ph_free(blob);
    CloseHandle(f);
}

static void pio_close(void)
{
    for (int i = 0; i < M_N; i++)
        if (mh[i]) { p_close(mh[i]); mh[i] = NULL; }
    if (lib) FreeLibrary(lib);
    lib = NULL;
    p_version = NULL; p_open = NULL; p_load = NULL; p_exec = NULL; p_close = NULL;
    mchbar = pm_base = 0;
    pm_resolve_ms = pm_cache_ms = 0;
    pm_cache_ok = 0;
    is_amd = 0;
}

static int pio_open(void)
{
    if (lib) return 0;
    wchar_t path[MAX_PATH];
    pawnio_state = DRV_MISSING;
    if (!find_file(L"PawnIOLib.dll", path)) return -1;
    lib = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return -1;
    p_version = (pio_version_t)GetProcAddress(lib, "pawnio_version");
    p_open    = (pio_open_t)GetProcAddress(lib, "pawnio_open");
    p_load    = (pio_load_t)GetProcAddress(lib, "pawnio_load");
    p_exec    = (pio_exec_t)GetProcAddress(lib, "pawnio_execute");
    p_close   = (pio_close_t)GetProcAddress(lib, "pawnio_close");
    if (!p_open || !p_load || !p_exec || !p_close) { pio_close(); return -1; }

    ULONG ver = 0;
    if (p_version && SUCCEEDED(p_version(&ver))) {
        unsigned ma = (ver >> 16) & 0xFF, mi = (ver >> 8) & 0xFF, pa = ver & 0xFF;
        if (pa) ph_snprintf(pawnio_ver, sizeof pawnio_ver, "%u.%u.%u", ma, mi, pa);
        else ph_snprintf(pawnio_ver, sizeof pawnio_ver, "%u.%u", ma, mi);
        ph_snprintf(pawnio_name, sizeof pawnio_name, "PawnIO %s", pawnio_ver);
    }

    HANDLE probe = NULL;
    if (FAILED(p_open(&probe)) || !probe) {
        ph_log("pawnio: driver not available");
        pio_close();
        pawnio_state = DRV_STOPPED;
        return -1;
    }
    p_close(probe);

    uint32_t r[4];
    cpuidex(0, 0, r);
    int intel = r[1] == 0x756e6547 && r[3] == 0x49656e69 && r[2] == 0x6c65746e;
    is_amd = r[1] == 0x68747541 && r[3] == 0x69746e65 && r[2] == 0x444d4163;
    cpuidex(1, 0, r);
    uint32_t fam = (r[0] >> 8) & 0xF;
    if (fam == 0xF) fam += (r[0] >> 20) & 0xFF;

    pawnio_missing[0] = 0;
    if (intel) {
        load_module(M_MSR, L"IntelMSR.bin");
        load_module(M_MCHBAR, L"IntelMCHBAR.bin");
        uint64_t a = 0;
        if (exec(M_MCHBAR, "ioctl_get_mchbar_addr", NULL, 0, &a, 1) == 0) mchbar = a;
        if (!mh[M_MSR]) note_missing("IntelMSR.bin");
    } else if (is_amd) {
        if (fam >= 0x17) load_module(M_MSR, L"AMDFamily17.bin");
        load_module(M_SMU, L"RyzenSMU.bin");
        if (fam >= 0x17 && !mh[M_MSR]) note_missing("AMDFamily17.bin");
        if (!mh[M_SMU]) note_missing("RyzenSMU.bin");
    }
    load_module(M_EC, L"LpcACPIEC.bin");

    for (int i = 0; i < M_N; i++)
        if (mh[i]) { pawnio_state = DRV_OK; return 0; }
    ph_log("pawnio: no modules loaded");
    pio_close();
    pawnio_state = DRV_NOMODULES;
    return -1;
}

static int set_cpu(int cpu, GROUP_AFFINITY *old)
{
    WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; g++) {
        int n = (int)GetActiveProcessorCount(g);
        if (cpu < n) {
            GROUP_AFFINITY ga;
            ZeroMemory(&ga, sizeof ga);
            ga.Group = g;
            ga.Mask = (KAFFINITY)1 << cpu;
            if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, old)) return -1;
            SwitchToThread();
            return 0;
        }
        cpu -= n;
    }
    return -1;
}

static int msr_op(uint32_t msr, int cpu, uint64_t *v, int wr)
{
    if (!mh[M_MSR]) return -1;
    GROUP_AFFINITY old;
    if (cpu >= 0 && set_cpu(cpu, &old) != 0) return -1;
    int rc;
    if (wr) {
        uint64_t in[2] = { msr, *v };
        rc = exec(M_MSR, "ioctl_write_msr", in, 2, NULL, 0);
    } else {
        uint64_t in = msr, out = 0;
        rc = exec(M_MSR, "ioctl_read_msr", &in, 1, &out, 1);
        if (rc == 0) *v = out;
    }
    if (cpu >= 0) SetThreadGroupAffinity(GetCurrentThread(), &old, NULL);
    return rc;
}

static int pio_rdmsr(uint32_t msr, int cpu, uint64_t *v) { return msr_op(msr, cpu, v, 0); }
static int pio_wrmsr(uint32_t msr, int cpu, uint64_t v)  { return msr_op(msr, cpu, &v, 1); }

static int smu_range(uint32_t a)
{
    return (a & 0xFFFFF000u) == 0x03B10000u || (a >= 0x56000u && a <= 0x5AFFFu);
}

static int smn_rd(uint32_t a, uint32_t *v)
{
    uint64_t in = a, out = 0;
    int rc = -1;
    if (mh[M_SMU] && smu_range(a)) rc = exec(M_SMU, "ioctl_read_smu_register", &in, 1, &out, 1);
    if (rc != 0 && is_amd && mh[M_MSR]) rc = exec(M_MSR, "ioctl_read_smn", &in, 1, &out, 1);
    if (rc == 0) *v = (uint32_t)out;
    return rc;
}

static int smn_wr(uint32_t a, uint32_t v)
{
    if (!mh[M_SMU] || !smu_range(a)) return -1;
    uint64_t in[2] = { a, v };
    pm_cache_ok = 0;
    return exec(M_SMU, "ioctl_write_smu_register", in, 2, NULL, 0);
}

static int smn_avail(void)
{
    return is_amd && (mh[M_SMU] || mh[M_MSR]);
}

static int pio_pci_rd(uint32_t bdf, uint32_t off, uint32_t *v)
{
    if (bdf != 0) return -1;
    switch (off) {
    case 0x60: case 0xB8:
        if (!smn_avail()) return -1;
        *v = smn_index;
        return 0;
    case 0x64: case 0xBC:
        if (!smn_avail()) return -1;
        return smn_rd(smn_index, v);
    case 0x48:
        if (!mchbar) return -1;
        *v = (uint32_t)mchbar | 1u;
        return 0;
    case 0x4C:
        if (!mchbar) return -1;
        *v = (uint32_t)(mchbar >> 32);
        return 0;
    }
    return -1;
}

static int pio_pci_wr(uint32_t bdf, uint32_t off, uint32_t v)
{
    if (bdf != 0 || !smn_avail()) return -1;
    if (off == 0x60 || off == 0xB8) { smn_index = v; return 0; }
    if (off == 0x64 || off == 0xBC) return smn_wr(smn_index, v);
    return -1;
}

static int pm_fetch(uint64_t pa)
{
    uint64_t now = ph_ms();
    if (!pm_base || pa < pm_base || pa >= pm_base + sizeof pm_cache) {
        if (pm_resolve_ms && now - pm_resolve_ms < 2000) return -1;
        pm_resolve_ms = now;
        uint64_t out[2] = { 0, 0 };
        if (exec(M_SMU, "ioctl_resolve_pm_table", NULL, 0, out, 2) != 0 || !out[1]) return -1;
        pm_base = out[1];
        pm_cache_ok = 0;
        if (pa < pm_base || pa >= pm_base + sizeof pm_cache) return -1;
    }
    if (!pm_cache_ok || now - pm_cache_ms > 250) {
        if (exec(M_SMU, "ioctl_read_pm_table", NULL, 0, pm_cache, PH_ARRAY(pm_cache)) != 0) return -1;
        pm_cache_ok = 1;
        pm_cache_ms = now;
    }
    return 0;
}

static int mch_rd(uint64_t off, uint64_t *v, uint32_t unit)
{
    uint64_t in = off & ~(uint64_t)(unit == 8 ? 7 : 3), out = 0;
    int rc = exec(M_MCHBAR, unit == 8 ? "ioctl_read_qword" : "ioctl_read_dword", &in, 1, &out, 1);
    if (rc) return rc;
    if (unit < 4) out >>= (off & 3) * 8;
    *v = out;
    return 0;
}

static int pio_mem_rd(uint64_t pa, void *buf, uint32_t unit, uint32_t count)
{
    if ((unit != 1 && unit != 2 && unit != 4 && unit != 8) || !count || (pa & (unit - 1))) return -1;
    uint8_t *b = buf;
    uint64_t len = (uint64_t)unit * count;
    if (mh[M_MCHBAR] && mchbar && pa >= mchbar && pa + len <= mchbar + 0x20000) {
        for (uint32_t i = 0; i < count; i++) {
            uint64_t v;
            if (mch_rd(pa - mchbar + (uint64_t)i * unit, &v, unit)) return -1;
            memcpy(b + (size_t)i * unit, &v, unit);
        }
        return 0;
    }
    if (mh[M_SMU] && pm_fetch(pa) == 0 && pa + len <= pm_base + sizeof pm_cache) {
        memcpy(b, (const uint8_t *)pm_cache + (pa - pm_base), (size_t)len);
        return 0;
    }
    return -1;
}

static int pio_mem_wr(uint64_t pa, const void *buf, uint32_t unit, uint32_t count)
{
    return -1;
}

static int pio_io_rd8(uint16_t port, uint8_t *v)
{
    if (!mh[M_EC] || (port != 0x62 && port != 0x66)) return -1;
    uint64_t in = port, out = 0;
    if (exec(M_EC, "ioctl_pio_read", &in, 1, &out, 1)) return -1;
    *v = (uint8_t)out;
    return 0;
}

static int pio_io_wr8(uint16_t port, uint8_t v)
{
    if (!mh[M_EC] || (port != 0x62 && port != 0x66)) return -1;
    uint64_t in[2] = { port, v };
    return exec(M_EC, "ioctl_pio_write", in, 2, NULL, 0);
}

const ph_drv drv_pawnio = {
    "pawnio",
    pio_open,
    pio_close,
    pio_rdmsr,
    pio_wrmsr,
    pio_pci_rd,
    pio_pci_wr,
    pio_mem_rd,
    pio_mem_wr,
    pio_io_rd8,
    pio_io_wr8,
};
