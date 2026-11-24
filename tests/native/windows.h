/* Just enough of <windows.h> to build the portable parts of Phawx ON with the
   native compiler for tests/logic_test.c. The functions are in logic_test.c;
   profile files are kept in memory. */
#ifndef PH_TEST_WINDOWS_H
#define PH_TEST_WINDOWS_H
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>

typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned int UINT;
typedef void *HANDLE, *HWND, *HINSTANCE, *HKEY, *PVOID, *LPVOID;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef wchar_t WCHAR;
typedef const char *LPCSTR;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef struct EXCEPTION_POINTERS EXCEPTION_POINTERS;
typedef struct { int dummy; } CRITICAL_SECTION;
typedef struct { int done; } INIT_ONCE, *PINIT_ONCE;
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);

#define TRUE  1
#define FALSE 0
#define CALLBACK
#define WINAPI
#define MAX_PATH 260
#define WM_APP 0x8000
#define CP_UTF8 65001
#define INIT_ONCE_STATIC_INIT { 0 }
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_WRITE 0x40000000
#define CREATE_NEW 1
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define OPEN_ALWAYS 4
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
typedef struct { uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

static inline int lstrlenW(const wchar_t *s) { return s ? (int)wcslen(s) : 0; }
static inline wchar_t *lstrcpynW(wchar_t *d, const wchar_t *s, int n)
{
    if (n <= 0) return d;
    wcsncpy(d, s, (size_t)n - 1);
    d[n - 1] = 0;
    return d;
}
static inline wchar_t *lstrcpyW(wchar_t *d, const wchar_t *s) { return wcscpy(d, s); }
static inline int lstrcmpW(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
static inline int lstrcmpiW(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
static inline int lstrcmpA(const char *a, const char *b) { return strcmp(a, b); }
static inline int lstrlenA(const char *s) { return (int)strlen(s); }
static inline char *lstrcpyA(char *d, const char *s) { return strcpy(d, s); }
static inline char *lstrcpynA(char *d, const char *s, int n)
{
    if (n <= 0) return d;
    strncpy(d, s, (size_t)n - 1);
    d[n - 1] = 0;
    return d;
}
#define _wcsnicmp wcsncasecmp

static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline BOOL InitOnceExecuteOnce(PINIT_ONCE o, PINIT_ONCE_FN f, PVOID p, PVOID *c)
{
    if (!o->done) { o->done = 1; f(o, p, c); }
    return TRUE;
}
static inline LONG InterlockedExchange(volatile LONG *t, LONG v) { LONG o = *t; *t = v; return o; }

/* in logic_test.c */
DWORD GetModuleFileNameW(HANDLE m, wchar_t *b, DWORD n);
HANDLE CreateFileW(const wchar_t *n, DWORD a, DWORD s, void *sa, DWORD c, DWORD f, HANDLE t);
BOOL CloseHandle(HANDLE h);
DWORD GetLastError(void);
DWORD GetEnvironmentVariableW(const wchar_t *n, wchar_t *b, DWORD sz);
BOOL CreateDirectoryW(const wchar_t *p, void *sa);
BOOL WriteFile(HANDLE h, const void *b, DWORD n, DWORD *w, void *ov);
void GetLocalTime(SYSTEMTIME *t);
int WideCharToMultiByte(UINT cp, DWORD fl, const wchar_t *w, int nw, char *a, int na, const char *d, BOOL *u);
int MultiByteToWideChar(UINT cp, DWORD fl, const char *a, int na, wchar_t *w, int nw);
DWORD GetPrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD n, const wchar_t *file);
BOOL WritePrivateProfileStringW(const wchar_t *sec, const wchar_t *key, const wchar_t *v, const wchar_t *file);
DWORD GetPrivateProfileSectionW(const wchar_t *sec, wchar_t *out, DWORD n, const wchar_t *file);
BOOL WritePrivateProfileSectionW(const wchar_t *sec, const wchar_t *data, const wchar_t *file);
DWORD GetPrivateProfileSectionNamesW(wchar_t *out, DWORD n, const wchar_t *file);
#endif
