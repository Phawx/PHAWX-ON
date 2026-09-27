# Per-process FPS / frametime capture (native C, Win32, mingw-w64)

Goal: one elevated background process measures the foreground game's application frame rate and per-frame times with less than 100 ms latency and close to zero CPU. Primary path: an ETW real-time session with the minimum set of providers and event-ID filters. Secondary path: RTSS shared memory, when RTSS is running.

Sources (checked 2026-09):
- PresentMon `PresentData/ETW/Microsoft_Windows_{DXGI,D3D9,DxgKrnl,Win32k}.h`, `PresentMonTraceSession.cpp`, `PresentMonTraceConsumer.cpp`: https://github.com/GameTechDev/PresentMon (main)
- PresentMon service manual flush: `IntelPresentMon/PresentMonService/RealtimePresentMonSession.cpp` (`FlushEvents()`)
- Windows SDK `evntrace.h`, `evntprov.h`, `evntcons.h` (win32metadata RecompiledIdlHeaders mirror)
- RTSS SDK `RTSSSharedMemory.h` (v2.x layout), mirrored at https://github.com/RecursiveLife/RTSS_Crosshair and https://github.com/Kaldaien/BMF
- RTSSHooks profile API usage: https://gist.github.com/MoreOrLessSoftware/7ab6eb4881059070b23a68fc58ca9dc2, https://github.com/xanderfrangos/rtss-cli

---

## 1. ETW real-time frame capture

### 1.1 Providers, GUIDs, keywords, events

All event IDs below come from PresentMon's generated manifest headers. The `EVENT_DESCRIPTOR_DECL(name, id, version, channel, level, opcode, task, keyword)` values are copied exactly.

```c
/* {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} Microsoft-Windows-DXGI */
static const GUID GUID_DXGI   = {0xCA11C036,0x0102,0x4A2D,{0xA6,0xAD,0xF0,0x3C,0xFE,0xD5,0xD3,0xC9}};
/* {783ACA0A-790E-4D7F-8451-AA850511C6B9} Microsoft-Windows-D3D9 */
static const GUID GUID_D3D9   = {0x783ACA0A,0x790E,0x4D7F,{0x84,0x51,0xAA,0x85,0x05,0x11,0xC6,0xB9}};
/* {802EC45A-1E99-4B83-9920-87C98277BA9D} Microsoft-Windows-DxgKrnl */
static const GUID GUID_DXGKRNL= {0x802EC45A,0x1E99,0x4B83,{0x99,0x20,0x87,0xC9,0x82,0x77,0xBA,0x9D}};
/* {8C416C79-D49B-4F01-A467-E56D3AA8234C} Microsoft-Windows-Win32k (not needed for frame counting) */
static const GUID GUID_WIN32K = {0x8C416C79,0xD49B,0x4F01,{0xA4,0x67,0xE5,0x6D,0x3A,0xA8,0x23,0x4C}};
```

| Provider | Event | ID (dec / hex) | Ver | Level | Opcode | Keyword in manifest |
|---|---|---|---|---|---|---|
| DXGI | Present_Start | 42 / 0x2A | 0 | 0 | 1 | 0x8000000000000002 |
| DXGI | Present_Stop | 43 / 0x2B | 0 | 0 | 2 | 0x8000000000000002 |
| DXGI | PresentMultiplaneOverlay_Start | 55 / 0x37 | 0 | 0 | 1 | 0x8000000000000002 |
| DXGI | PresentMultiplaneOverlay_Stop | 56 / 0x38 | 0 | 0 | 2 | 0x8000000000000002 |
| D3D9 | Present_Start | 1 | 0 | 0 | 1 | 0x8000000000000002 |
| D3D9 | Present_Stop | 2 | 0 | 0 | 2 | 0x8000000000000002 |
| DxgKrnl | MMIOFlip_Info | 116 / 0x74 | 0 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | VSyncDPC_Info | 17 / 0x11 | 0 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | Blit_Info | 166 / 0xA6 | 0 | 4 | 0 | 0x4000000008000001 |
| DxgKrnl | Flip_Info | 168 / 0xA8 | 0 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | PresentHistory_Start | **171 / 0xAB** | 2 | 0 | 1 | 0x4000000008000001 |
| DxgKrnl | PresentHistory_Info | 172 / 0xAC | 2 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | QueuePacket_Start / _Stop | 178 / 180 | 1 | 0 | 1/2 | 0x4000000000000001 |
| DxgKrnl | Present_Info | 184 / 0xB8 | 1 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | PresentHistoryDetailed_Start | 215 / 0xD7 | 2 | 0 | 1 | 0x4000000008000001 |
| DxgKrnl | FlipMultiPlaneOverlay_Info | 252 / 0xFC | 0 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | MMIOFlipMultiPlaneOverlay_Info | 259 / 0x103 | 3 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | IndependentFlip_Info | 266 / 0x10A | 3 | 0 | 0 | 0x4000000000000001 |
| DxgKrnl | VSyncDPCMultiPlane_Info | 273 / 0x111 | 4 | 0 | 0 | 0x4000000008000001 |
| DxgKrnl | HSyncDPCMultiPlane_Info | 382 / 0x17E | 2 | 0 | 0 | 0x4000000008000001 |
| Win32k | TokenCompositionSurfaceObject_Info | 201 / 0xC9 | 1 | 4 | 0 | 0x8000000400001000 |
| Win32k | TokenStateChanged_Info | 301 / 0x12D | 1 | 4 | 0 | 0x8000000000001000 |

Corrections to the task brief: PresentHistory_Start is **171**, not 215. 215 is PresentHistoryDetailed_Start, which PresentMon enables only when it tracks display. Blit is 166, Flip is 168, MMIOFlip is 116 and VSyncDPC is 17.

Keyword rules. These mirror PresentMon's `PatchKeyword` and `PatchPreWin11Keyword`.
- DxgKrnl: always clear the channel bit `0x4000000000000000` (Microsoft_Windows_DxgKrnl_Performance) from the manifest keyword.
- DxgKrnl on builds earlier than Windows 11 (build < 22000): also clear the `Present` keyword `0x8000000`. The event then uses only `Base` (0x1).
  - Windows 11 keyword for PresentHistory_Start: `0x0000000008000001`.
  - Windows 10 keyword for PresentHistory_Start: `0x0000000000000001`.
- DXGI and D3D9: use `0x8000000000000002` (Events plus the Analytic channel bit) unchanged.
- PresentMon passes `MatchAnyKeyword = OR` and `MatchAllKeyword = AND` of all requested events' keywords. For a single keyword value this means any = all = that value.
- Level: every frame event is level 0 (LogAlways) except Blit_Info. Enable with `TRACE_LEVEL_INFORMATION` (4).
- **Always attach an `EVENT_FILTER_TYPE_EVENT_ID` filter.** Supported on Windows 8.1 and later; the runtime drops unlisted events before they are written.
  - This matters most for DxgKrnl. `Base` (0x1) alone also enables the very high-rate DmaPacket and QueuePacket events.
  - Without the filter, CPU cost rises by orders of magnitude.

Payload layouts (fields in order, from PresentMon's `EventDataDesc` lookups):
- DXGI Present_Start (42) and PresentMultiplaneOverlay_Start (55):
  - `pIDXGISwapChain` (pointer), `Flags` (UINT32, DXGI_PRESENT_*), `SyncInterval` (INT32).
- D3D9 Present_Start (1):
  - `pSwapchain` (pointer), `Flags` (UINT32, D3DPRESENT_*).
- DXGI and D3D9 Present_Stop:
  - `Result` (UINT32 HRESULT).
- DxgKrnl PresentHistory_Start (171, v2):
  - PresentMon reads `Token` (UINT64), `Model` (UINT32, D3DKMT_PRESENT_MODEL) and `TokenData` (UINT64).
  - The Win7 classic struct is `{ULONGLONG hAdapter; ULONGLONG Token; ULONG Model; UINT TokenSize;}` (pack 1).
  - **Uncertain:** that the v2 manifest keeps `hAdapter, Token, Model` first, which would put `Model` at offset 16. Resolve the offset once with TDH (§1.5).
  - Skip `Model == 1` (D3DKMT_PM_REDIRECTED_GDI), as PresentMon does.
  - Model enum: 0 UNINITIALIZED, 1 REDIRECTED_GDI, 2 REDIRECTED_FLIP, 3 REDIRECTED_BLT, 4 REDIRECTED_VISTABLT, 5 SCREENCAPTUREFENCE, 6 REDIRECTED_GDI_SYSMEM, 7 REDIRECTED_COMPOSITION, 8 SURFACECOMPLETE, 9 FLIPMANAGER.
- Pointer size depends on the process that logged the event. Test `EventHeader.Flags`:
  - `EVENT_HEADER_FLAG_32_BIT_HEADER` (0x0020) means 4-byte pointers (a WOW64 32-bit game).
  - `EVENT_HEADER_FLAG_64_BIT_HEADER` (0x0040) means 8-byte pointers.
  - So DXGI `Flags` is at `UserData + ptrSize`. Always check `UserDataLength`.
- Ignore DXGI presents with `Flags & DXGI_PRESENT_TEST (0x1)`. They are occlusion and fullscreen probes, not frames; PresentMon drops them in `RuntimePresentStart`.
- `EventHeader.ProcessId` and `ThreadId` on all of these events identify the presenting app's process and thread. For DxgKrnl this holds because the kernel present runs on the caller's thread; PresentMon relies on the same fact (`FindOrCreatePresent(hdr)`).

How each API shows up:
- **D3D10/11/12 and DX9Ex through DXGI.** DXGI Present_Start, followed on the same thread by DxgKrnl PresentHistory_Start for flip and blt-model presents.
- **Legacy exclusive fullscreen (FSE).** DXGI Present_Start plus DxgKrnl Flip_Info (168) and MMIOFlip. There is **no** PresentHistory in this mode.
- **D3D9.** D3D9 Present_Start (1), plus DxgKrnl events.
- **Vulkan, OpenGL, DXVK and other APIs** (PresentMon's `Runtime::Other`). No runtime event exists.
  - Frames appear only as DxgKrnl PresentHistory_Start from the game PID. This holds for AMD and Intel Vulkan, most OpenGL, and DXVK.
  - The NVIDIA option "Vulkan/OpenGL present method: Prefer layered on DXGI swapchain" routes presents through DXGI, so they appear as DXGI events.
  - OpenGL or Vulkan in true exclusive fullscreen can use legacy flip: Flip_Info only, with no PresentHistory. **Uncertain.**
- **Driver frame generation (AMD AFMF, Intel/NVIDIA driver-level).** Generated frames do not produce runtime Present events, so the counts above are *application* frames, which is what the control loop wants.
  - DLSS-FG and FSR3-FG inside the game do call Present for generated frames. Expect those counts to be about 2x the rendered rate.
  - PresentMon separates them using the Nvidia_PCL and FrameType events. Not needed for v1; document this for users.

Win32k TokenStateChanged (301) and the Dwm-Core events are needed only for *displayed* time and composition state, not for application FPS. Omit them. They add Win32k and DWM traffic.

### 1.2 Frame-counting rule per PID (no double counting)

Use a runtime-first latch. It is simple, robust, and needs no Present_Stop events.

```
on DXGI 42/55 (Flags&1 == 0) or D3D9 1:
    p = pid_slot(pid); p->last_runtime_qpc = ts; frame(p, ts)
on DxgKrnl 171 (Model != 1):
    p = pid_slot(pid);
    if (ts - p->last_runtime_qpc > 1 s) frame(p, ts)            /* Vulkan/GL/other */
optional DxgKrnl 168 Flip_Info:
    if (ts - p->last_runtime_qpc > 1 s && ts - p->last_hist_qpc > 1 s) frame(p, ts)
```

- A DXGI or D3D9 app always emits its runtime event before the kernel present. Once the latch is set, kernel events from that PID are ignored.
  - This also covers threaded or batched drivers that issue the kernel present on a worker thread after Present_Stop, where a per-thread rule would double count.
  - It also covers FSE, which has no PresentHistory event.
- When the game starts, at most about one kernel event is counted before the first runtime event sets the latch. This is negligible.
- A tighter per-thread alternative, PresentMon style: track per-TID nesting between Present_Start and Present_Stop, and count kernel presents only at depth 0. Do not use it. It double counts under threaded drivers unless you add PresentMon's fallback that matches by process.
- Skip PIDs that are never the game: your own PID, `dwm.exe`, `csrss.exe`, `explorer.exe`. Resolve names lazily, only when a PID first reaches the frame threshold.

### 1.3 Session setup (C, mingw-w64)

mingw-w64 `evntprov.h` and `evntrace.h` lack the event-ID filter definitions. Define them locally. Values come from the SDK `evntprov.h`.

```c
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
/* link: -ladvapi32 (-ltdh only if 1.5 used) */

#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200
#define EVENT_FILTER_TYPE_PID      0x80000004  /* max 8 PIDs; not used, see notes */
#define MAX_EVENT_FILTER_EVENT_ID_COUNT 64
typedef struct { BOOLEAN FilterIn; UCHAR Reserved; USHORT Count; USHORT Events[1]; } EVENT_FILTER_EVENT_ID;
#endif
#ifndef EVENT_HEADER_FLAG_32_BIT_HEADER
#define EVENT_HEADER_FLAG_32_BIT_HEADER 0x0020
#endif

#define FPS_SESSION L"PhawxON-FPS"

typedef struct { EVENT_TRACE_PROPERTIES p; WCHAR name[64]; } SessProps;

static TRACEHANDLE g_sess, g_trace = INVALID_PROCESSTRACE_HANDLE;

static void props_init(SessProps *s) {
    ZeroMemory(s, sizeof *s);
    s->p.Wnode.BufferSize    = sizeof *s;
    s->p.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    s->p.Wnode.ClientContext = 1;          /* 1 = QPC timestamps, 2 = system time, 3 = CPU cycle */
    s->p.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    s->p.BufferSize          = 16;         /* KB */
    s->p.MinimumBuffers      = 4;
    s->p.MaximumBuffers      = 32;
    s->p.FlushTimer          = 1;          /* seconds; minimum. Real latency is set by manual flush (1.4) */
    s->p.LoggerNameOffset    = offsetof(SessProps, name);
}

static ULONG enable_ids(const GUID *g, ULONGLONG kw, const USHORT *ids, USHORT n) {
    BYTE buf[sizeof(EVENT_FILTER_EVENT_ID) + sizeof(USHORT) * 16];
    EVENT_FILTER_EVENT_ID *f = (EVENT_FILTER_EVENT_ID *)buf;
    f->FilterIn = TRUE; f->Reserved = 0; f->Count = n;
    for (USHORT i = 0; i < n; i++) f->Events[i] = ids[i];
    EVENT_FILTER_DESCRIPTOR d = { (ULONGLONG)(ULONG_PTR)f,
        (ULONG)(sizeof(EVENT_FILTER_EVENT_ID) + sizeof(USHORT) * (n - 1)), EVENT_FILTER_TYPE_EVENT_ID };
    ENABLE_TRACE_PARAMETERS ep = {0};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &d;
    ep.FilterDescCount = 1;
    return EnableTraceEx2(g_sess, g, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, kw, kw, 0, &ep);
}

static void WINAPI on_event(PEVENT_RECORD r);

static ULONG fps_etw_start(BOOL win11) {
    SessProps s; props_init(&s);
    ULONG st = StartTraceW(&g_sess, FPS_SESSION, &s.p);
    if (st == ERROR_ALREADY_EXISTS) {               /* stale session from a crash */
        SessProps k; props_init(&k);
        ControlTraceW(0, FPS_SESSION, &k.p, EVENT_TRACE_CONTROL_STOP);
        props_init(&s);
        st = StartTraceW(&g_sess, FPS_SESSION, &s.p);
    }
    if (st != ERROR_SUCCESS) return st;              /* ERROR_ACCESS_DENIED: not admin/PLU;
                                                        ERROR_NO_SYSTEM_RESOURCES: 64 sessions in use */
    static const USHORT kIds[]   = { 171 };          /* PresentHistory_Start; add 168 if Flip_Info fallback wanted */
    static const USHORT dxgiIds[]= { 42, 55 };
    static const USHORT d9Ids[]  = { 1 };
    ULONGLONG kkw = win11 ? 0x0000000008000001ULL : 0x1ULL;
    if ((st = enable_ids(&GUID_DXGKRNL, kkw, kIds, 1)) ||           /* backend first, like PresentMon */
        (st = enable_ids(&GUID_DXGI, 0x8000000000000002ULL, dxgiIds, 2)) ||
        (st = enable_ids(&GUID_D3D9, 0x8000000000000002ULL, d9Ids, 1))) goto fail;

    EVENT_TRACE_LOGFILEW lf = {0};
    lf.LoggerName = FPS_SESSION;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD
                        | PROCESS_TRACE_MODE_RAW_TIMESTAMP;   /* TimeStamp stays raw QPC */
    lf.EventRecordCallback = on_event;
    g_trace = OpenTraceW(&lf);
    if (g_trace == INVALID_PROCESSTRACE_HANDLE) { st = GetLastError(); goto fail; }
    return ERROR_SUCCESS;                             /* then run ProcessTrace on its own thread */
fail:
    { SessProps k; props_init(&k); ControlTraceW(g_sess, NULL, &k.p, EVENT_TRACE_CONTROL_STOP); }
    g_sess = 0; return st;
}

static DWORD WINAPI etw_thread(LPVOID u) {
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ProcessTrace(&g_trace, 1, NULL, NULL);            /* blocks until CloseTrace/session stop */
    return 0;
}

static void fps_etw_stop(void) {
    SessProps k; props_init(&k);
    if (g_sess) ControlTraceW(g_sess, NULL, &k.p, EVENT_TRACE_CONTROL_STOP);
    if (g_trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(g_trace);  /* ProcessTrace returns */
    g_sess = 0; g_trace = INVALID_PROCESSTRACE_HANDLE;
}
```

Callback. Keep it allocation-free and lock-free, and push `(pid, qpc)` only for frames.

```c
static void WINAPI on_event(PEVENT_RECORD r) {
    const EVENT_HEADER *h = &r->EventHeader;
    USHORT id = h->EventDescriptor.Id;
    ULONG pid = h->ProcessId;
    LONGLONG ts = h->TimeStamp.QuadPart;              /* QPC ticks: compare with QueryPerformanceCounter */
    UINT ps = (h->Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    const BYTE *u = (const BYTE *)r->UserData; USHORT n = r->UserDataLength;

    if (IsEqualGUID(&h->ProviderId, &GUID_DXGI)) {
        if ((id == 42 || id == 55) && n >= ps + 4 && !(*(const UINT32 *)(u + ps) & 1)) runtime_frame(pid, ts);
    } else if (IsEqualGUID(&h->ProviderId, &GUID_D3D9)) {
        if (id == 1) runtime_frame(pid, ts);
    } else if (IsEqualGUID(&h->ProviderId, &GUID_DXGKRNL)) {
        if (id == 171 && n >= g_model_off + 4 && *(const UINT32 *)(u + g_model_off) != 1) kernel_frame(pid, ts);
    }
}
```

`runtime_frame` and `kernel_frame` implement §1.2 over a small open-addressing table of about 64 PID slots. Each slot holds `last_runtime_qpc` and a ring of timestamps; §4 covers only the target PID's ring. `g_model_off` defaults to 16 and is verified via §1.5. Compare GUIDs as two 64-bit loads to save cycles.

Notes:
- **Privileges.** Starting a session and enabling a kernel provider (DxgKrnl) needs Administrators or membership in *Performance Log Users*. The app is elevated, so this is fine.
- **Session names are global.** A real-time session **survives the process** when the process crashes and keeps its buffers, and 64 sessions are allowed system-wide.
  - Always stop a stale session with the same name at startup (above).
  - Also stop it from `SetConsoleCtrlHandler`/`WM_ENDSESSION` and from an unhandled-exception filter.
- **Coexistence.** PresentMon, FrameView, RTSS, Xbox Game Bar and CapFrameX run their own sessions. A manifest provider can be enabled in up to 8 sessions at once. Use a distinct name and do not `ControlTrace STOP` other names.
- **PID filter.** `EVENT_FILTER_TYPE_PID` is capped at 8 PIDs and is applied at enable time, so a foreground change requires re-enabling. It is also documented as ineffective for kernel-mode providers such as DxgKrnl (**uncertain**). Filter in the callback instead; the event-ID filter already keeps the rate at about 1–2 events per frame system-wide.
- `PROCESS_TRACE_MODE_RAW_TIMESTAMP` with `ClientContext=1` gives raw QPC in `TimeStamp`. Without the raw flag, ETW converts timestamps to FILETIME. PresentMon uses the same combination.
- `ProcessTrace` returns once the session is stopped or `CloseTrace` is called. Join the thread afterwards.

### 1.4 Latency: force delivery every ~50 ms

Real-time buffers reach the consumer only when a buffer fills or `FlushTimer` fires, and the minimum FlushTimer is 1 s. The public SDK has no millisecond flush-timer flag; `evntrace.h` documents FlushTimer "in seconds". At about 2–50 KB/s of frame events, a 16 KB buffer can wait close to 1 s. PresentMon's service solves this with periodic manual flushes, and we do the same:

```c
static void fps_etw_flush(void) {               /* call from control-loop timer, every 50 ms */
    SessProps k; props_init(&k);
    ControlTraceW(g_sess, NULL, &k.p, EVENT_TRACE_CONTROL_FLUSH);   /* = 3 */
}
```

- Run it only while AutoTDP or the OSD needs live FPS. Otherwise either stop the session or skip the flush and let the 1 s FlushTimer suffice.
- `EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING` (0x10000000; the SDK comment reads "use this for low frequency sessions") keeps one buffer pool. Partial buffers are then not spread across N CPUs, so each flush delivers fewer and fuller buffers.
- End-to-end latency after this change is flush period plus consumer wake-up, typically ≤ 50–60 ms. **Estimate:** not measured here.

### 1.5 Resolving `Model` offset once with TDH (optional, robust)

The first time event 171 arrives with a given `EventDescriptor.Version`, call `TdhGetEventInformation(r, 0, NULL, info, &size)`. Walk `info->EventPropertyInfoArray[i]`, where the name is at `(BYTE*)info + NameOffset`, and add up the sizes of the fixed-size fields before `Model`:

| InType | Size |
|---|---|
| `TDH_INTYPE_POINTER` | kernel pointer size, 8 on x64 |
| `UINT64` / `HEXINT64` | 8 |
| `UINT32` / `HEXINT32` | 4 |

Cache the offset per version. Cost is a single call. If lookup fails, fall back to 16. If `Model` is missing, disable the GDI filter by setting `g_model_off` past `UserDataLength` so the length check fails, and count every 171 event.

### 1.6 CPU cost (estimates)

- Provider side: roughly 1–3 µs per enabled event written.
  - Filtered session: about 2–3 events per frame for the game plus the desktop's DWM and other presenters. At 144 fps that is about 500 events/s, or 0.1 % of one core or less.
- Consumer side: 20 flush syscalls/s plus the callback, which is trivial.
- PresentMon with full display and GPU tracking typically uses 1–3 % of a core. The large difference comes from its extra DxgKrnl events: QueuePacket, DmaPacket, MMIOFlip and VSyncDPC.
- Stop the session when AutoTDP and the FPS readout are both off.

---

## 2. RTSS shared memory ("RTSSSharedMemoryV2")

Open read-only with `OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2")` and `MapViewOfFile`. Re-open when the signature changes or RTSS restarts. The header uses **default (8-byte) packing**; there is no `#pragma pack`.

Header offsets:

| Off | Field | Notes |
|---|---|---|
| 0 | `DWORD dwSignature` | `'RTSS'` = 0x52545353 when valid; 0xDEAD = being freed |
| 4 | `DWORD dwVersion` | (major<<16)\|minor; require `>= 0x00020000` |
| 8 | `DWORD dwAppEntrySize` | Use this as the stride. The header comments are swapped with the OSD ones. |
| 12 | `DWORD dwAppArrOffset` | |
| 16 | `DWORD dwAppArrSize` | Number of app slots, normally 256 |
| 20 | `DWORD dwOSDEntrySize` | |
| 24 | `DWORD dwOSDArrOffset` | |
| 28 | `DWORD dwOSDArrSize` | Normally 8 |
| 32 | `DWORD dwOSDFrame` | |
| 36 | `LONG dwBusy` | v2.14 and later; bit 0 is the writer lock, for OSD writers only |

App entry at `base + dwAppArrOffset + i*dwAppEntrySize`. Offsets were computed from the v2.x header with default alignment:

```c
typedef struct {               /* offsets */
    DWORD dwProcessID;         /*   0  0 = free slot */
    char  szName[260];         /*   4  full exe path */
    DWORD dwFlags;             /* 264  APPFLAG_*: low 16 bits API (1 OGL,2 DD,3 D3D8,4 D3D9,5 D3D9EX,6 D3D10,
                                        7 D3D11,8 D3D12,9 D3D12AFR,0xA VULKAN), 0x10000 x64, 0x20000 UWP */
    DWORD dwTime0;             /* 268  ms, start of 1 s measurement period */
    DWORD dwTime1;             /* 272  ms, end of period */
    DWORD dwFrames;            /* 276  frames in [dwTime0,dwTime1] */
    DWORD dwFrameTime;         /* 280  us, last frame time */
    DWORD dwStatFlags;         /* 284  STATFLAG_RECORD=1 */
    DWORD dwStatTime0, dwStatTime1, dwStatFrames, dwStatCount;          /* 288..300 */
    DWORD dwStatFramerateMin, dwStatFramerateAvg, dwStatFramerateMax;   /* 304..312 */
    /* 316.. OSD/capture fields ... */
} RTSS_APP_HEAD;
/* v2.5+:  dwStatFrameTimeMin 908, Avg 912, Max 916, Count 920,
           dwStatFrameTimeBuf[1024] 924, dwStatFrameTimeBufPos 5020, dwStatFrameTimeBufFramerate 5024
   v2.13+: qwStatTotalTime 5072, dwStatFrameTimeLowBuf[1024] 5080,
           dwStatFramerate1Dot0PercentLow 9176, dwStatFramerate0Dot1PercentLow 9180
   sizeof(v2.13+ entry) = 9184 (use dwAppEntrySize, never sizeof) */
```

- FPS formulas from the SDK:
  - Averaged over about 1 s: `1000.0 * dwFrames / (dwTime1 - dwTime0)`. `dwTime0` must be non-zero.
  - Per frame: `1e6 / dwFrameTime`.
- For a 100 ms control loop, sample `dwFrameTime` at 20–50 Hz and filter it yourself (§4). The `dwFrames` window updates only about once per second.
- The `dwStatFrameTimeBuf` ring (1024 entries, index `dwStatFrameTimeBufPos`) is filled only while stats recording (`STATFLAG_RECORD`) is active. **Uncertain.** Do not depend on it.
- Pick the entry whose `dwProcessID` equals the target PID. The same PID can appear in more than one slot; keep the one with the latest `dwTime1`.
- Caveats:
  - RTSS counts frames only for processes it has hooked. Stealth mode, anti-cheat blocks and excluded apps are not counted.
  - Its hook adds some CPU and GPU overhead in the game itself.
  - Use RTSS as a fallback or a second opinion, not as the primary path.

Setting a frame-rate limit through `RTSSHooks64.dll`, loaded from the RTSS install folder:
- Registry location: `HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS`, value `InstallDir`. **Uncertain** on the value name.
- Default path: `C:\Program Files (x86)\RivaTuner Statistics Server\RTSSHooks64.dll`.

```c
typedef BOOL (__stdcall *pGetProp)(LPCSTR name, LPVOID data, DWORD size);
typedef BOOL (__stdcall *pSetProp)(LPCSTR name, LPVOID data, DWORD size);
typedef void (__stdcall *pLoadProf)(LPCSTR profile);   /* "" = Global, else "game.exe" */
typedef void (__stdcall *pSaveProf)(LPCSTR profile);
typedef void (__stdcall *pUpdProf)(void);
/* sequence: LoadProfile(p); SetProfileProperty("FramerateLimit",&dw,4); SaveProfile(p); UpdateProfiles(); */
```

- Export names: `LoadProfile`, `SaveProfile`, `GetProfileProperty`, `SetProfileProperty`, `UpdateProfiles`.
- A value of 0 disables the limit.
- Fractional limits use a denominator property. Both `"FramerateLimitDenominator"` and `"LimitDenominator"` appear in community code; the guru3D thread uses `LimitDenominator`. **Uncertain.**
- The whole API is undocumented and reverse-engineered from the Afterburner and RTSS SDK samples. Expect it to change.
- Licensing: the RTSS SDK headers are © Unwinder and come with Afterburner and RTSS.
  - Do not redistribute `RTSSHooks64.dll`.
  - Treat the header as reference only. Define our own structs from the offsets above, and use RTSS only if the user installed it.
- Writing `dwStatFlags` or other entry fields does **not** set a frame limit. Only the profile API does.

---

## 3. Foreground game detection

Signals, cheapest first:
1. `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, cb, 0, 0, WINEVENT_OUTOFCONTEXT)`
   - Event driven; needs a message loop on the hooking thread.
   - In the callback: `GetWindowThreadProcessId(hwnd, &pid)`.
2. **UWP / Store games.** The foreground window belongs to `ApplicationFrameHost.exe`.
   - `EnumChildWindows(hwnd, ...)` and take the child whose PID differs (class `Windows.UI.Core.CoreWindow`).
   - Better still, use the "presenting PID" rule below, which handles this automatically.
3. The presenting table from §1 or RTSS. **The target PID must be presenting**: at least 10 frames in the last 1 s.
4. Fullscreen hints:
   - `SHQueryUserNotificationState(&q)` (shell32, Vista+). Values: `QUNS_RUNNING_D3D_FULL_SCREEN = 3` for exclusive fullscreen D3D, `QUNS_BUSY = 2` for a fullscreen app or presentation mode, `QUNS_PRESENTATION_MODE = 4`, `QUNS_ACCEPTS_NOTIFICATIONS = 5`, `QUNS_QUIET_TIME = 6`, `QUNS_APP = 7`.
   - Borderless-windowed games usually report 2 or 5, so treat this only as a hint.
   - Geometric test: `GetWindowRect(hwnd)` equals `GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).rcMonitor`, and the style lacks `WS_CAPTION`.

Target selection, re-evaluated about every 250 ms and on every foreground event:

```
fg = foreground PID (resolved through ApplicationFrameHost)
if fg presents >= 10 fps over the last 1 s           -> target = fg
else if fg is our own PID (QAM overlay open)
     or fg is in the ignore list                     -> keep previous target while it still presents (sticky)
else if previous target presented in the last 2 s    -> keep it (alt-tab, launcher overlay, Steam overlay)
else target = highest-rate presenter that is not in the ignore list
             (dwm, explorer, csrss, ShellExperienceHost, StartMenuExperienceHost,
              TextInputHost, SearchHost, our exe, steamwebhelper, browsers optional)
             and that has at least 20 fps for at least 2 s; otherwise none (AutoTDP idles)
```

- **Launchers.** A game launched by a launcher (Steam, EA, Ubisoft, Battle.net, or a stub exe) renders in a different PID from the launcher. The presenting rule selects the real renderer.
  - Launcher UIs also present (CEF/Chromium via DXGI), usually at low or irregular rates. Require the target to present steadily, and prefer the fullscreen or foreground presenter.
- **PID reuse.** Store the process creation time with the PID: `GetProcessTimes` on an `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` handle. Alternatively keep the handle and wait on it; `WaitForSingleObject` returning signals exit.
- Name: `QueryFullProcessImageNameW`. Resolve it only once per new PID, for per-game profiles.
- **The QAM overlay must never become "the game".** Exclude our own PID, and make the overlay a `WS_EX_NOACTIVATE` topmost window where possible so the game keeps the foreground.

---

## 4. Control-loop signals

Storage:
- An SPSC ring of 512 QPC timestamps for the target PID. The ETW thread writes and the control thread reads with `InterlockedExchange` or acquire loads on the head index.
- Frametime `ft_i = (t_i - t_{i-1}) / qpf` in ms.

Signals, all computed on the control thread at a 50 ms tick after `fps_etw_flush()`:

| Signal | Definition | Purpose |
|---|---|---|
| `fps_fast` | frames in last `max(150 ms, 6 frames)` / span | fast drop detection |
| `ft_ema_fast` | EMA of ft, τ = 150 ms: `a = 1 - exp(-ft/τ)` per frame | smooth fast |
| `fps_slow` | frames in last 1.0 s / span (`(N-1)/(t_last - t_first)`) | "target met" test, UI |
| `p99_ft` (1 % low) | 99th percentile of ft over last 3 s; 1 % low fps = 1000/p99_ft | stutter guard |
| `spike` | `ft_i > 2.0 × target_ft` (or > target_ft + 8 ms) | immediate boost trigger |
| `stall` | no frame for > max(250 ms, 5 × target_ft) | loading, menu or pause: freeze controller, don't down-clock |

- Compute percentiles in O(1) with a histogram: 0.25 ms bins from 0–64 ms, 256 bins, plus an overflow bin. Maintain it incrementally by adding the new frame and removing the frame that ages out of the 3 s ring (1024 entries covers 3 s at up to 340 fps).
- Use the median of the last 3 frametimes as the per-frame input. This rejects single-frame outliers such as shader compiles and GC hitches in the fast path, while `spike` still reports them.

Decision rules. The loop is asymmetric: it goes up fast and comes down slow (AIMD-like).
- `target_ft = 1000/target_fps`. Deadband: met means `fps_slow >= 0.97 × target`.
- **Boost (≤ 100 ms)** when any of these is true:
  - `fps_fast < 0.93 × target`,
  - 2 consecutive `spike` frames,
  - `1000/p99_ft < 0.80 × target` over the last 1 s.

  Response: raise clocks by a *large* step, for example +15–25 % of range or a jump back to the last known-good level. Then apply a step-down cooldown of 2–3 s.
- **Reduce** only if `met` has held for ≥ 1.5 s (dwell) *and* no boost occurred in the last cooldown window. Step by −3 to −5 % of range, one step per dwell.
- **Anti-oscillation memory.** Record the clock level at which the last boost happened (the "floor"). Do not step below `floor + 1 step` for 20–30 s. Decay the floor after that so scene changes can reclaim headroom.
- **Frame cap interaction.** When the game or RTSS caps FPS at the target, FPS cannot rise above target, so FPS alone cannot show headroom.
  - The controller must *probe*: keep stepping down while met, and back off one or two steps on the first sustained dip. This works and is what AutoTDP-style tools do.
  - For better headroom estimates, also read GPU busy % (vendor APIs) and CPU busy per core.
  - Time inside Present (DXGI 42 to 43 on the same TID) grows under GPU-bound backpressure or vsync. RTSS's limiter sleeps *before* calling Present, outside that interval. **Uncertain** as a general signal; enable 43 only if you use it.
  - Recommend enabling a frame cap at the target, via the game's own limiter or RTSS. Without a cap, FPS above target just wastes power and the controller keeps pushing down until the deadband.
- Window sizes, as a trade-off:
  - Fast window 100–150 ms reacts within about 3 frames at 30 fps. Shorter windows at 30 fps give ≤ 3 samples and turn noisy.
  - The 1 s slow window plus 1.5 s dwell prevents limit cycles.
  - The 3 s window for 1 % lows follows CapFrameX/PresentMon convention, shortened for control.
- **Scene-change and stall guard.** On `stall`, target change, or a foreground change, reset the rings and hold clocks for 1 s before acting. Loading screens often run uncapped (very high FPS) or stall; neither should drive clocks down.

Pitfalls checklist:
- Discard `DXGI_PRESENT_TEST`.
- Do not add DXGI and DxgKrnl counts for the same PID (§1.2 latch).
- DWM (`dwm.exe`) presents at the refresh rate and must never be chosen as the target.
- VRR/G-Sync does not change application-frame counting.
- With vsync on, measured FPS clamps at refresh or refresh/2. Set the target at or below the effective cap.
- Frame generation inflates DXGI counts by 2x for in-game FG (DLSS-FG, FSR3-FG) but not for driver FG (AFMF). Let the user give the target as either the "rendered" or the "output" rate; auto-detecting FG is out of scope for v1.
- 32-bit games: pointer size in the payload is 4 (§1.1).
- **Session leaks.** Always `ControlTraceW(... STOP)` by name on startup and on exit. After too many leaked sessions, StartTrace fails with `ERROR_NO_SYSTEM_RESOURCES`.
