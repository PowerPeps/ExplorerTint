#include "RibbonTab.h"
#include "ImportPatch.h"
#include "RibbonPatch.h"
#include <ole2.h>
#include <commctrl.h>
#include <initguid.h>
#include <UIRibbon.h>
#include <UIRibbonPropertyHelpers.h>
#include <map>

namespace {

const wchar_t kResName[] = L"EXPLORER_RIBBON";
const wchar_t kResType[] = L"UIFILE";

struct OwnCommand {
    WORD id;
    const wchar_t* label;
    bool boolean;
};

const OwnCommand kOwn[] = {
    { 0x7E00, L"ExplorerTint", false },
    { 0x7E01, L"Tint", false },
    { 0x7E02, L"Couleurs Git", true },
    { 0x7E03, L"Système en dernier", true },
    { 0x7E04, L"Noms masqués", true },
    { 0x7E05, L"Paramètres", false },
};

const OwnCommand* Own(UINT32 id) {
    for (auto& c : kOwn)
        if (c.id == id) return &c;
    return nullptr;
}

typedef HRESULT(STDMETHODCALLTYPE* InitializeFn)(IUIFramework*, HWND, IUIApplication*);
typedef HRESULT(STDMETHODCALLTYPE* DestroyFn)(IUIFramework*);
typedef HRESULT(STDMETHODCALLTYPE* CreateCommandFn)(IUIApplication*, UINT32, UI_COMMANDTYPE, IUICommandHandler**);

INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
volatile LONG s_enabled = 0;
std::vector<BYTE> s_blob;
SRWLOCK s_lock = SRWLOCK_INIT;
HRSRC s_res = nullptr;
HGLOBAL s_glob = nullptr;
InitializeFn s_initialize = nullptr;
DestroyFn s_destroy = nullptr;
CreateCommandFn s_createCommand = nullptr;
void** s_appVtable = nullptr;
std::map<UINT32, bool> s_state;
std::map<HWND, IUIFramework*> s_hosts;     // ribbon host window -> its framework
UINT s_refreshMessage = 0;
const UINT_PTR kSubclassId = 0x7E00;

bool Serving() { return s_enabled && !s_blob.empty(); }

bool IsRibbonName(LPCWSTR name, LPCWSTR type) {
    return !IS_INTRESOURCE(name) && !IS_INTRESOURCE(type) && !_wcsicmp(name, kResName) && !_wcsicmp(type, kResType);
}

HRSRC WINAPI HookFindResourceW(HMODULE m, LPCWSTR name, LPCWSTR type) {
    HRSRC r = FindResourceW(m, name, type);
    if (r && IsRibbonName(name, type)) s_res = r;
    return r;
}

HRSRC WINAPI HookFindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang) {
    HRSRC r = FindResourceExW(m, type, name, lang);
    if (r && IsRibbonName(name, type)) s_res = r;
    return r;
}

DWORD WINAPI HookSizeofResource(HMODULE m, HRSRC r) {
    if (r && r == s_res && Serving()) return (DWORD)s_blob.size();
    return SizeofResource(m, r);
}

HGLOBAL WINAPI HookLoadResource(HMODULE m, HRSRC r) {
    HGLOBAL g = LoadResource(m, r);
    if (r && r == s_res) s_glob = g;
    return g;
}

LPVOID WINAPI HookLockResource(HGLOBAL g) {
    if (g && g == s_glob && Serving()) {
        Log(L"ribbon: patched markup served");
        return s_blob.data();
    }
    return LockResource(g);
}

class Handler : public IUICommandHandler {
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IUICommandHandler)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP Execute(UINT32 id, UI_EXECUTIONVERB verb, const PROPERTYKEY*, const PROPVARIANT*, IUISimplePropertySet*) override {
        const OwnCommand* own = Own(id);
        if (!own || verb != UI_EXECUTIONVERB_EXECUTE) return S_OK;
        Log(L"ribbon: execute 0x%04X", id);
        if (own->boolean) {
            AcquireSRWLockExclusive(&s_lock);
            s_state[id] = !s_state[id];
            std::vector<HWND> hosts;
            for (auto& h : s_hosts) hosts.push_back(h.first);
            ReleaseSRWLockExclusive(&s_lock);
            for (HWND h : hosts) PostMessageW(h, s_refreshMessage, 0, 0);
        }
        return S_OK;
    }

    IFACEMETHODIMP UpdateProperty(UINT32 id, REFPROPERTYKEY key, const PROPVARIANT*, PROPVARIANT* value) override {
        const OwnCommand* own = Own(id);
        if (!own) return E_NOTIMPL;
        if (key == UI_PKEY_Label) return UIInitPropertyFromString(UI_PKEY_Label, own->label, value);
        if (key == UI_PKEY_BooleanValue && own->boolean) {
            AcquireSRWLockShared(&s_lock);
            bool state = s_state[id];
            ReleaseSRWLockShared(&s_lock);
            return UIInitPropertyFromBoolean(UI_PKEY_BooleanValue, state, value);
        }
        return E_NOTIMPL;
    }
} s_handler;

HRESULT STDMETHODCALLTYPE HookCreateCommand(IUIApplication* app, UINT32 id, UI_COMMANDTYPE type, IUICommandHandler** out) {
    if (Own(id) && out) {
        Log(L"ribbon: command 0x%04X created (type %d)", id, type);
        *out = &s_handler;
        return S_OK;
    }
    return s_createCommand(app, id, type, out);
}

bool PatchSlot(void** vtable, int slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void ForgetHost(HWND hwnd) {
    RemoveWindowSubclass(hwnd, HostProc, kSubclassId);
    AcquireSRWLockExclusive(&s_lock);
    s_hosts.erase(hwnd);
    ReleaseSRWLockExclusive(&s_lock);
}

// Runs on each ribbon host thread, where its framework may be called.
LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == s_refreshMessage && s_refreshMessage) {
        auto fw = (IUIFramework*)ref;
        for (auto& c : kOwn)
            if (c.boolean) fw->InvalidateUICommand(c.id, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_BooleanValue);
        return 0;
    }
    if (msg == WM_NCDESTROY) ForgetHost(hwnd);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HRESULT STDMETHODCALLTYPE HookInitialize(IUIFramework* fw, HWND hwnd, IUIApplication* app) {
    if (app) {
        void** vtable = *(void***)app;
        AcquireSRWLockExclusive(&s_lock);
        if (!s_appVtable) {
            CreateCommandFn original = (CreateCommandFn)vtable[4];
            if (PatchSlot(vtable, 4, (void*)HookCreateCommand)) {
                s_createCommand = original;
                s_appVtable = vtable;
            }
        }
        bool known = vtable == s_appVtable;
        ReleaseSRWLockExclusive(&s_lock);
        if (!known) Log(L"ribbon: unexpected application vtable %p", vtable);
    }
    HRESULT hr = s_initialize(fw, hwnd, app);
    if (SUCCEEDED(hr) && hwnd && SetWindowSubclass(hwnd, HostProc, kSubclassId, (DWORD_PTR)fw)) {
        AcquireSRWLockExclusive(&s_lock);
        s_hosts[hwnd] = fw;
        ReleaseSRWLockExclusive(&s_lock);
    }
    Log(L"ribbon: framework initialized for window %p (0x%08X)", hwnd, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDestroy(IUIFramework* fw) {
    HWND host = nullptr;
    AcquireSRWLockShared(&s_lock);
    for (auto& h : s_hosts)
        if (h.second == fw) host = h.first;
    ReleaseSRWLockShared(&s_lock);
    if (host) ForgetHost(host);
    return s_destroy(fw);
}

BOOL CALLBACK Install(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE frame = GetModuleHandleW(L"ExplorerFrame.dll");
    HRSRC r = frame ? FindResourceW(frame, kResName, kResType) : nullptr;
    auto data = r ? (const BYTE*)LockResource(LoadResource(frame, r)) : nullptr;
    if (!data) {
        Log(L"ribbon: markup resource not found");
        return TRUE;
    }
    std::string error;
    std::vector<BYTE> blob;
    if (!RibbonPatch::AddTab(data, SizeofResource(frame, r), RibbonPatch::NewTab(), blob, &error)) {
        Log(L"ribbon: patch failed: %S", error.c_str());
        return TRUE;
    }

    HMODULE ui = LoadLibraryExW(L"UIRibbon.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ui) {
        Log(L"ribbon: UIRibbon.dll not loaded");
        return TRUE;
    }
    IUIFramework* fw = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(UIRibbonFramework), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fw)))) {
        Log(L"ribbon: framework not available");
        return TRUE;
    }
    void** vtable = *(void***)fw;
    s_initialize = (InitializeFn)vtable[3];
    s_destroy = (DestroyFn)vtable[4];
    bool hooked = PatchSlot(vtable, 3, (void*)HookInitialize) && PatchSlot(vtable, 4, (void*)HookDestroy);
    fw->Release();
    if (!hooked) {
        Log(L"ribbon: framework vtable not patched");
        return TRUE;
    }

    s_refreshMessage = RegisterWindowMessageW(L"ExplorerTint.RibbonRefresh");
    s_blob = std::move(blob);
    int slots = PatchImports(ui, nullptr, "FindResourceW", (void*)HookFindResourceW)
              + PatchImports(ui, nullptr, "FindResourceExW", (void*)HookFindResourceExW)
              + PatchImports(ui, nullptr, "SizeofResource", (void*)HookSizeofResource)
              + PatchImports(ui, nullptr, "LoadResource", (void*)HookLoadResource)
              + PatchImports(ui, nullptr, "LockResource", (void*)HookLockResource);
    Log(L"ribbon: hooks installed (%d import slots, markup %zu bytes)", slots, s_blob.size());
    return TRUE;
}

}

void InstallRibbonTab(bool enabled) {
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
    if (enabled) InitOnceExecuteOnce(&s_once, Install, nullptr, nullptr);
}
