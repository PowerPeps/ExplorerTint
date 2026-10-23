#include "ClassicList.h"
#include "ImportPatch.h"
#include <objbase.h>

static const CLSID CLSID_ItemsView =
    { 0x1eeb5b5a, 0x06fb, 0x4732, { 0x96, 0xb3, 0x97, 0x5c, 0x01, 0x94, 0xeb, 0x39 } };

typedef HRESULT (WINAPI* CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HRESULT (WINAPI* CoCreateInstanceEx_t)(REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD, MULTI_QI*);
typedef HRESULT (WINAPI* CoGetClassObject_t)(REFCLSID, DWORD, LPVOID, REFIID, LPVOID*);

static CoCreateInstance_t s_coCreateInstance;
static CoCreateInstanceEx_t s_coCreateInstanceEx;
static CoGetClassObject_t s_coGetClassObject;
static volatile LONG s_enabled;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static bool Refuse(REFCLSID clsid, const wchar_t* api) {
    if (!s_enabled || clsid != CLSID_ItemsView) return false;
    Log(L"classic list: Itemsview refused in %s", api);
    return true;
}

static HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* ppv) {
    if (Refuse(clsid, L"CoCreateInstance")) {
        if (ppv) *ppv = nullptr;
        return REGDB_E_CLASSNOTREG;
    }
    return s_coCreateInstance(clsid, outer, ctx, iid, ppv);
}

static HRESULT WINAPI HookCoCreateInstanceEx(REFCLSID clsid, IUnknown* outer, DWORD ctx, COSERVERINFO* server,
                                             DWORD count, MULTI_QI* results) {
    if (Refuse(clsid, L"CoCreateInstanceEx")) {
        for (DWORD i = 0; i < count; i++) {
            results[i].pItf = nullptr;
            results[i].hr = REGDB_E_CLASSNOTREG;
        }
        return REGDB_E_CLASSNOTREG;
    }
    return s_coCreateInstanceEx(clsid, outer, ctx, server, count, results);
}

static HRESULT WINAPI HookCoGetClassObject(REFCLSID clsid, DWORD ctx, LPVOID info, REFIID iid, LPVOID* ppv) {
    if (Refuse(clsid, L"CoGetClassObject")) {
        if (ppv) *ppv = nullptr;
        return REGDB_E_CLASSNOTREG;
    }
    return s_coGetClassObject(clsid, ctx, info, iid, ppv);
}

static BOOL CALLBACK Install(PINIT_ONCE, void*, void**) {
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (!combase) combase = LoadLibraryW(L"combase.dll");
    s_coCreateInstance = (CoCreateInstance_t)GetProcAddress(combase, "CoCreateInstance");
    s_coCreateInstanceEx = (CoCreateInstanceEx_t)GetProcAddress(combase, "CoCreateInstanceEx");
    s_coGetClassObject = (CoGetClassObject_t)GetProcAddress(combase, "CoGetClassObject");
    if (!s_coCreateInstance || !s_coCreateInstanceEx || !s_coGetClassObject) {
        Log(L"classic list: combase exports not found");
        return TRUE;
    }

    const wchar_t* modules[] = { L"shell32.dll", L"windows.storage.dll", L"explorerframe.dll" };
    for (const wchar_t* m : modules) {
        HMODULE h = GetModuleHandleW(m);
        if (!h) {
            Log(L"classic list: %s not loaded", m);
            continue;
        }
        int n = PatchImports(h, nullptr, "CoCreateInstance", (void*)HookCoCreateInstance);
        n += PatchImports(h, nullptr, "CoCreateInstanceEx", (void*)HookCoCreateInstanceEx);
        n += PatchImports(h, nullptr, "CoGetClassObject", (void*)HookCoGetClassObject);
        Log(L"classic list: %d import slot(s) patched in %s", n, m);
    }
    return TRUE;
}

void InstallClassicList(bool enabled) {
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
    InitOnceExecuteOnce(&s_once, Install, nullptr, nullptr);
}
