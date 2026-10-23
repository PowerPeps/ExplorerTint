#include "ClassicList.h"
#include <objbase.h>
#include <cstring>

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

static void PatchSlot(ULONG_PTR* slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return;
    *slot = (ULONG_PTR)hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static int PatchThunks(BYTE* base, IMAGE_THUNK_DATA* names, IMAGE_THUNK_DATA* slots, const char* name, void* hook) {
    int count = 0;
    for (; names->u1.AddressOfData; names++, slots++) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
        auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
        if (strcmp((const char*)byName->Name, name) != 0) continue;
        PatchSlot(&slots->u1.Function, hook);
        count++;
    }
    return count;
}

static int PatchImports(HMODULE mod, const char* name, void* hook) {
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    int count = 0;

    auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress) {
        for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress); d->Name; d++) {
            if (!d->OriginalFirstThunk) continue;
            count += PatchThunks(base, (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk),
                                 (IMAGE_THUNK_DATA*)(base + d->FirstThunk), name, hook);
        }
    }

    auto& dly = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dly.VirtualAddress) {
        for (auto d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dly.VirtualAddress); d->DllNameRVA; d++) {
            if (!d->Attributes.RvaBased) continue;
            count += PatchThunks(base, (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA),
                                 (IMAGE_THUNK_DATA*)(base + d->ImportAddressTableRVA), name, hook);
        }
    }
    return count;
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
        int n = PatchImports(h, "CoCreateInstance", (void*)HookCoCreateInstance);
        n += PatchImports(h, "CoCreateInstanceEx", (void*)HookCoCreateInstanceEx);
        n += PatchImports(h, "CoGetClassObject", (void*)HookCoGetClassObject);
        Log(L"classic list: %d import slot(s) patched in %s", n, m);
    }
    return TRUE;
}

void InstallClassicList(bool enabled) {
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
    InitOnceExecuteOnce(&s_once, Install, nullptr, nullptr);
}
