// Generated export stubs call this lazily, outside DllMain. We load only the
// absolute System32 DLL and preserve every named and ordinal export.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "proxy_exports.h"

static HMODULE g_real;
static void* g_targets[PROXY_EXPORT_COUNT];

// Lock-free on purpose: a once-lock here can deadlock against the loader lock.
// Thread B takes the once-lock and waits in LoadLibrary for the loader lock,
// while thread A, holding the loader lock (a DllMain, TLS callback or static
// import), calls an export and waits for B's once-lock. Without the once-lock
// A's LoadLibrary re-enters the loader lock it owns and B simply waits for it.
// Racing loads only add a reference to the same module.
static HMODULE LoadSystem(void) {
    HMODULE module = (HMODULE)InterlockedCompareExchangePointer((PVOID*)&g_real, NULL, NULL);
    if (module) return module;
    WCHAR path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 32) return NULL;
    lstrcatW(path, L"\\"); lstrcatW(path, PROXY_SYSTEM_DLL);
    module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return NULL;
    HMODULE previous = (HMODULE)InterlockedCompareExchangePointer((PVOID*)&g_real, module, NULL);
    return previous ? previous : module;
}

extern "C" void* ProxyResolve(unsigned index) {
    HMODULE real = index < PROXY_EXPORT_COUNT ? LoadSystem() : NULL;
    if (!real) { RaiseFailFastException(NULL, NULL, 0); return NULL; }
    void* function = InterlockedCompareExchangePointer(&g_targets[index], NULL, NULL);
    if (!function) {
        function = (void*)GetProcAddress(real, (LPCSTR)(ULONG_PTR)kProxyOrdinals[index]);
        if (!function) { RaiseFailFastException(NULL, NULL, 0); return NULL; }
        InterlockedCompareExchangePointer(&g_targets[index], function, NULL);
    }
    return function;
}
