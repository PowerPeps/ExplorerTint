#include "RibbonTab.h"
#include "ImportPatch.h"
#include "RibbonPatch.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include <ole2.h>
#include <commctrl.h>
#include <shlobj.h>
#include <initguid.h>
#include <UIRibbon.h>
#include <UIRibbonPropertyHelpers.h>
#include <propvarutil.h>
#include <map>

namespace {

const wchar_t kResName[] = L"EXPLORER_RIBBON";
const wchar_t kResType[] = L"UIFILE";
const int kSettingsIcon = 316;  // shell32.dll icon index: gear

struct OwnCommand {
    WORD id;
    const wchar_t* fr;
    const wchar_t* en;
    const wchar_t* tipFr;
    const wchar_t* tipEn;
    const wchar_t* value;       // registry value of a checkbox
    bool Settings::* field;
};

const OwnCommand kOwn[] = {
    { 0x7E00, L"ExplorerTint", L"ExplorerTint", nullptr, nullptr, nullptr, nullptr },
    { 0x7E01, L"Tint", L"Tint", nullptr, nullptr, nullptr, nullptr },
    { 0x7E02, L"Couleurs Git", L"Git colors",
      L"Colore les noms selon leur état Git (indexé, non suivi, ignoré).",
      L"Colors names by their Git state (staged, untracked, ignored).", L"Git", &Settings::git },
    { 0x7E03, L"Système en dernier", L"System items last",
      L"Place les éléments système (cachés et système) après les autres, quel que soit le tri.",
      L"Sorts system items (hidden and system) after the others, whatever the sort order.", L"SystemLast",
      &Settings::systemLast },
    { 0x7E04, L"Noms masqués", L"Hidden names",
      L"Affiche les noms listés (.git, .idea…) comme des éléments cachés.",
      L"Shows the listed names (.git, .idea…) as hidden items.", L"HideNames", &Settings::hideNames },
    { 0x7E05, L"Paramètres", L"Settings", L"Couleurs, opacité, noms masqués et options.",
      L"Colors, opacity, hidden names and options.", nullptr, nullptr },
};

const OwnCommand* Own(UINT32 id) {
    for (auto& c : kOwn)
        if (c.id == id) return &c;
    return nullptr;
}

const wchar_t* Tr(const wchar_t* fr, const wchar_t* en) {
    return UiFrench() ? fr : en;
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
std::map<HWND, IUIFramework*> s_hosts;     // ribbon host window -> its framework
UINT s_openMessage = 0;
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

HWND HostOfThisThread() {
    HWND found = nullptr;
    DWORD me = GetCurrentThreadId();
    AcquireSRWLockShared(&s_lock);
    for (auto& h : s_hosts)
        if (GetWindowThreadProcessId(h.first, nullptr) == me) found = h.first;
    ReleaseSRWLockShared(&s_lock);
    return found;
}

// 32 bpp DIB with premultiplied alpha, as drawn by DrawIconEx on a cleared surface.
IUIImage* SettingsImage(bool large) {
    HWND host = HostOfThisThread();
    UINT dpi = host ? GetDpiForWindow(host) : 96;
    int size = MulDiv(large ? 32 : 16, dpi ? dpi : 96, 96);
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, ARRAYSIZE(path));
    wcscat_s(path, L"\\shell32.dll");
    HICON icon = nullptr;
    if (SHDefExtractIconW(path, kSettingsIcon, 0, &icon, nullptr, (UINT)size) != S_OK || !icon) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    DestroyIcon(icon);
    if (!bmp) return nullptr;

    IUIImageFromBitmap* factory = nullptr;
    IUIImage* image = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(UIRibbonImageFromBitmapFactory), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)))) {
        if (FAILED(factory->CreateImage(bmp, UI_OWNERSHIP_TRANSFER, &image))) image = nullptr;
        factory->Release();
    }
    if (!image) DeleteObject(bmp);
    return image;
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

    IFACEMETHODIMP Execute(UINT32 id, UI_EXECUTIONVERB verb, const PROPERTYKEY* key, const PROPVARIANT* value,
                           IUISimplePropertySet*) override {
        const OwnCommand* own = Own(id);
        if (!own || verb != UI_EXECUTIONVERB_EXECUTE) return S_OK;
        if (own->field) {
            BOOL on = !(CurrentSettings().get()->*own->field);
            if (key && *key == UI_PKEY_BooleanValue && value) PropVariantToBoolean(*value, &on);
            Log(L"ribbon: %s set to %d", own->value, on);
            SaveSetting(own->value, on ? 1 : 0);
        } else if (id == 0x7E05) {
            if (HWND host = HostOfThisThread()) PostMessageW(host, s_openMessage, 0, 0);
        }
        return S_OK;
    }

    IFACEMETHODIMP UpdateProperty(UINT32 id, REFPROPERTYKEY key, const PROPVARIANT*, PROPVARIANT* value) override {
        const OwnCommand* own = Own(id);
        if (!own) return E_NOTIMPL;
        if (key == UI_PKEY_Label || key == UI_PKEY_TooltipTitle)
            return UIInitPropertyFromString(key, Tr(own->fr, own->en), value);
        if (key == UI_PKEY_TooltipDescription && own->tipFr)
            return UIInitPropertyFromString(key, Tr(own->tipFr, own->tipEn), value);
        if (key == UI_PKEY_BooleanValue && own->field)
            return UIInitPropertyFromBoolean(key, CurrentSettings().get()->*own->field, value);
        if ((key == UI_PKEY_LargeImage || key == UI_PKEY_SmallImage) && (id == 0x7E05 || id == 0x7E01)) {
            IUIImage* image = SettingsImage(key == UI_PKEY_LargeImage);
            if (!image) return E_FAIL;
            HRESULT hr = UIInitPropertyFromImage(key, image, value);
            image->Release();
            return hr;
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
    RemoveSettingsListener(hwnd);
    AcquireSRWLockExclusive(&s_lock);
    s_hosts.erase(hwnd);
    ReleaseSRWLockExclusive(&s_lock);
}

// Runs on each ribbon host thread, where its framework may be called.
LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == SettingsChangedMessage()) {
        auto fw = (IUIFramework*)ref;
        for (auto& c : kOwn)
            if (c.field) fw->InvalidateUICommand(c.id, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_BooleanValue);
        return 0;
    }
    if (msg == s_openMessage && s_openMessage) {
        ShowSettingsDialog(GetAncestor(hwnd, GA_ROOT));
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
        AddSettingsListener(hwnd);
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

    s_openMessage = RegisterWindowMessageW(L"ExplorerTint.OpenSettings");
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
