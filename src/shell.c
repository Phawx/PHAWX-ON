#define COBJMACROS
#include "phawx.h"
#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlobj.h>
#undef ShellExecute   /* the ANSI/Unicode macro would rename IShellDispatch2's method */

/* Phawx ON runs elevated, and ShellExecute from here would start the browser (and
   whatever it downloads) elevated too. Ask the desktop's Explorer to run it instead,
   which gives the user's normal token (Raymond Chen's IShellDispatch2 method). */
static int via_explorer(const wchar_t *target)
{
    IShellWindows *sw = NULL;
    IDispatch *disp = NULL, *bg = NULL, *app = NULL;
    IServiceProvider *sp = NULL;
    IShellBrowser *br = NULL;
    IShellView *sv = NULL;
    IShellFolderViewDual *fv = NULL;
    IShellDispatch2 *sd = NULL;
    VARIANT loc, empty, args, dir, op, show;
    BSTR file = NULL;
    long hwnd = 0;
    int ok = 0;

    VariantInit(&loc); VariantInit(&empty); VariantInit(&args);
    VariantInit(&dir); VariantInit(&op); VariantInit(&show);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    if (FAILED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_ALL, &IID_IShellWindows, (void **)&sw)) || !sw) goto out;
    if (IShellWindows_FindWindowSW(sw, &loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) goto out;
    if (FAILED(IDispatch_QueryInterface(disp, &IID_IServiceProvider, (void **)&sp))) goto out;
    if (FAILED(IServiceProvider_QueryService(sp, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&br))) goto out;
    if (FAILED(IShellBrowser_QueryActiveShellView(br, &sv)) || !sv) goto out;
    if (FAILED(IShellView_GetItemObject(sv, SVGIO_BACKGROUND, &IID_IDispatch, (void **)&bg)) || !bg) goto out;
    if (FAILED(IDispatch_QueryInterface(bg, &IID_IShellFolderViewDual, (void **)&fv))) goto out;
    if (FAILED(IShellFolderViewDual_get_Application(fv, &app)) || !app) goto out;
    if (FAILED(IDispatch_QueryInterface(app, &IID_IShellDispatch2, (void **)&sd))) goto out;
    file = SysAllocString(target);
    args.vt = dir.vt = op.vt = VT_BSTR;
    args.bstrVal = SysAllocString(L"");
    dir.bstrVal = SysAllocString(L"");
    op.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    ok = file && args.bstrVal && dir.bstrVal && op.bstrVal &&
         SUCCEEDED(IShellDispatch2_ShellExecute(sd, file, args, dir, op, show));
out:
    if (file) SysFreeString(file);
    VariantClear(&args); VariantClear(&dir); VariantClear(&op);
    if (sd) IShellDispatch2_Release(sd);
    if (app) IDispatch_Release(app);
    if (fv) IShellFolderViewDual_Release(fv);
    if (bg) IDispatch_Release(bg);
    if (sv) IShellView_Release(sv);
    if (br) IShellBrowser_Release(br);
    if (sp) IServiceProvider_Release(sp);
    if (disp) IDispatch_Release(disp);
    if (sw) IShellWindows_Release(sw);
    return ok;
}

static int to_clipboard(const wchar_t *s)
{
    size_t n = ((size_t)lstrlenW(s) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return -1;
    void *d = GlobalLock(g);
    if (!d) { GlobalFree(g); return -1; }
    CopyMemory(d, s, n);
    GlobalUnlock(g);
    if (!OpenClipboard(NULL)) { GlobalFree(g); return -1; }
    EmptyClipboard();
    int ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok ? 0 : -1;
}

static DWORD WINAPI open_thread(LPVOID p)
{
    wchar_t *target = p;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!(SUCCEEDED(co) && via_explorer(target))) {
        /* No Explorer desktop (e.g. a custom handheld shell). Starting the browser
           from here would run it as administrator, so hand over the link instead. */
        ph_log("shell: no explorer desktop to open %ls", target);
        wchar_t m[160];
        ph_swprintf(m, PH_ARRAY(m), to_clipboard(target) == 0 ? L"Copied to the clipboard: %s" : L"Open %s", target);
        ui_toast(m);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    ph_free(target);
    return 0;
}

int ph_open_url(const wchar_t *target)
{
    if (!target || !target[0]) return -1;
    int n = lstrlenW(target) + 1;
    wchar_t *copy = ph_alloc((size_t)n * sizeof(wchar_t));
    if (!copy) return -1;
    lstrcpyW(copy, target);
    HANDLE t = CreateThread(NULL, 0, open_thread, copy, 0, NULL);
    if (!t) { ph_free(copy); return -1; }
    CloseHandle(t);
    return 0;
}
