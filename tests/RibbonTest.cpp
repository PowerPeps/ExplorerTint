// RibbonTest.exe --check [--file blob] [--out patched.bin]
// RibbonTest.exe --show original|patched [--full] [--png prefix]
// RibbonTest.exe --probe [--toggle] [--png prefix]
#include "../src/RibbonPatch.h"
#include "../src/ImportPatch.h"
#include <ole2.h>
#include <initguid.h>
#include <UIRibbon.h>
#include <UIRibbonPropertyHelpers.h>
#include <UIAutomation.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <cstdio>
#include <map>
#include <set>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t kResName[] = L"EXPLORER_RIBBON";
const wchar_t kResType[] = L"UIFILE";

int g_failures = 0;

void Report(bool ok, const char* what) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

HMODULE LoadFrame(bool full) {
    if (full) return LoadLibraryExW(L"ExplorerFrame.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return LoadLibraryExW(L"ExplorerFrame.dll", nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

bool ReadResource(HMODULE frame, std::vector<BYTE>& out) {
    HRSRC r = FindResourceW(frame, kResName, kResType);
    if (!r) return false;
    DWORD size = SizeofResource(frame, r);
    auto p = (const BYTE*)LockResource(LoadResource(frame, r));
    if (!p || !size) return false;
    out.assign(p, p + size);
    return true;
}

bool ReadFileBytes(const wchar_t* path, std::vector<BYTE>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    BOOL ok = ReadFile(f, out.data(), size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

bool WriteFileBytes(const wchar_t* path, const std::vector<BYTE>& data) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(f);
    return ok && put == data.size();
}

void PrintStats(const char* label, const RibbonPatch::Stats& s) {
    printf("  %-9s size %zu, nodes %zu, collections %zu, pointers %zu, blocks %zu, entries %zu, tabs %zu\n",
        label, s.size, s.nodes, s.collections, s.pointers, s.blocks, s.entries, s.tabs);
}

int Check(const wchar_t* file, const wchar_t* outPath) {
    std::vector<BYTE> original;
    if (file) {
        Report(ReadFileBytes(file, original), "read blob file");
    } else {
        HMODULE frame = LoadFrame(false);
        Report(frame && ReadResource(frame, original), "read UIFILE/EXPLORER_RIBBON from ExplorerFrame.dll");
    }
    if (g_failures) return g_failures;

    std::string error;
    RibbonPatch::Stats before, after;
    bool ok = RibbonPatch::Validate(original.data(), original.size(), &before, &error);
    Report(ok, "parse original");
    if (!ok) { printf("  %s\n", error.c_str()); return g_failures; }
    PrintStats("original", before);

    std::vector<BYTE> rebuilt;
    ok = RibbonPatch::RoundTrip(original.data(), original.size(), rebuilt, &error);
    Report(ok && rebuilt == original, "rebuild original byte for byte");
    if (!ok) printf("  %s\n", error.c_str());

    RibbonPatch::NewTab tab;
    std::vector<BYTE> patched;
    ok = RibbonPatch::AddTab(original.data(), original.size(), tab, patched, &error);
    Report(ok, "add tab");
    if (!ok) { printf("  %s\n", error.c_str()); return g_failures; }

    ok = RibbonPatch::Validate(patched.data(), patched.size(), &after, &error);
    Report(ok, "parse patched");
    if (!ok) { printf("  %s\n", error.c_str()); return g_failures; }
    PrintStats("patched", after);

    Report(after.tabs == before.tabs + 1, "one more main tab");
    Report(after.blocks == before.blocks + 1, "one more block");
    Report(after.pointers == before.pointers + 1, "one more block pointer");
    Report(after.entries == before.entries + 2 + tab.controlIds.size(), "index entries for tab, group and controls");

    std::vector<BYTE> again;
    ok = RibbonPatch::RoundTrip(patched.data(), patched.size(), again, &error);
    Report(ok && again == patched, "rebuild patched byte for byte");

    std::vector<BYTE> twice;
    ok = RibbonPatch::AddTab(patched.data(), patched.size(), tab, twice, &error);
    Report(!ok, "refuse to add the same ids twice");

    if (outPath) Report(WriteFileBytes(outPath, patched), "write patched blob");
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}

// --show

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

std::vector<BYTE> g_blob;
bool g_serve = false;
HRSRC g_res = nullptr;
HGLOBAL g_glob = nullptr;
int g_hitFind = 0, g_hitSize = 0, g_hitLoad = 0, g_hitLock = 0;
std::set<UINT32> g_created;
std::map<UINT32, std::set<std::string>> g_updates;
std::map<UINT32, bool> g_state;
IUIFramework* g_fw = nullptr;
HMODULE g_frame = nullptr;
HRESULT g_loadHr = E_PENDING;
UINT32 g_ribbonHeight = 0;

bool IsRibbonName(LPCWSTR name, LPCWSTR type) {
    return !IS_INTRESOURCE(name) && !IS_INTRESOURCE(type) && !_wcsicmp(name, kResName) && !_wcsicmp(type, kResType);
}

HRSRC WINAPI HookFindResourceW(HMODULE m, LPCWSTR name, LPCWSTR type) {
    HRSRC r = FindResourceW(m, name, type);
    if (r && IsRibbonName(name, type)) { g_res = r; g_hitFind++; }
    return r;
}

HRSRC WINAPI HookFindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang) {
    HRSRC r = FindResourceExW(m, type, name, lang);
    if (r && IsRibbonName(name, type)) { g_res = r; g_hitFind++; }
    return r;
}

DWORD WINAPI HookSizeofResource(HMODULE m, HRSRC r) {
    if (r && r == g_res) {
        g_hitSize++;
        if (g_serve) return (DWORD)g_blob.size();
    }
    return SizeofResource(m, r);
}

HGLOBAL WINAPI HookLoadResource(HMODULE m, HRSRC r) {
    HGLOBAL g = LoadResource(m, r);
    if (r && r == g_res) { g_glob = g; g_hitLoad++; }
    return g;
}

LPVOID WINAPI HookLockResource(HGLOBAL g) {
    if (g && g == g_glob) {
        g_hitLock++;
        if (g_serve) return g_blob.data();
    }
    return LockResource(g);
}

void InstallResourceHooks() {
    HMODULE ui = GetModuleHandleW(L"UIRibbon.dll");
    if (!ui) { printf("  UIRibbon.dll not loaded\n"); return; }
    int n = PatchImports(ui, nullptr, "FindResourceW", (void*)HookFindResourceW)
          + PatchImports(ui, nullptr, "FindResourceExW", (void*)HookFindResourceExW)
          + PatchImports(ui, nullptr, "SizeofResource", (void*)HookSizeofResource)
          + PatchImports(ui, nullptr, "LoadResource", (void*)HookLoadResource)
          + PatchImports(ui, nullptr, "LockResource", (void*)HookLockResource);
    printf("  UIRibbon.dll import slots patched: %d\n", n);
}

const char* KeyName(REFPROPERTYKEY key) {
    if (key == UI_PKEY_Label) return "Label";
    if (key == UI_PKEY_BooleanValue) return "BooleanValue";
    if (key == UI_PKEY_Enabled) return "Enabled";
    if (key == UI_PKEY_LargeImage) return "LargeImage";
    if (key == UI_PKEY_SmallImage) return "SmallImage";
    if (key == UI_PKEY_TooltipTitle) return "TooltipTitle";
    if (key == UI_PKEY_TooltipDescription) return "TooltipDescription";
    if (key == UI_PKEY_Keytip) return "Keytip";
    return "other";
}

class Handler : public IUICommandHandler {
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IUICommandHandler)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP Execute(UINT32 id, UI_EXECUTIONVERB verb, const PROPERTYKEY* key, const PROPVARIANT*, IUISimplePropertySet*) override {
        const OwnCommand* own = Own(id);
        if (!own) return E_NOTIMPL;
        printf("  Execute 0x%04X verb %d\n", id, verb);
        if (own->boolean && verb == UI_EXECUTIONVERB_EXECUTE) {
            g_state[id] = !g_state[id];
            g_fw->InvalidateUICommand(id, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_BooleanValue);
        }
        return S_OK;
    }

    IFACEMETHODIMP UpdateProperty(UINT32 id, REFPROPERTYKEY key, const PROPVARIANT*, PROPVARIANT* value) override {
        const OwnCommand* own = Own(id);
        if (!own) return E_NOTIMPL;
        g_updates[id].insert(KeyName(key));
        if (key == UI_PKEY_Label) return UIInitPropertyFromString(UI_PKEY_Label, own->label, value);
        if (key == UI_PKEY_BooleanValue && own->boolean) return UIInitPropertyFromBoolean(UI_PKEY_BooleanValue, g_state[id], value);
        return E_NOTIMPL;
    }
} g_handler;

class Application : public IUIApplication {
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IUIApplication)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP OnViewChanged(UINT32, UI_VIEWTYPE, IUnknown* view, UI_VIEWVERB verb, INT32) override {
        IUIRibbon* ribbon = nullptr;
        if (view && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&ribbon)))) {
            ribbon->GetHeight(&g_ribbonHeight);
            ribbon->Release();
        }
        if (verb == UI_VIEWVERB_CREATE) printf("  ribbon view created\n");
        return S_OK;
    }

    IFACEMETHODIMP OnCreateUICommand(UINT32 id, UI_COMMANDTYPE type, IUICommandHandler** out) override {
        g_created.insert(id);
        if (Own(id)) printf("  OnCreateUICommand 0x%04X type %d\n", id, type);
        *out = &g_handler;
        return S_OK;
    }

    IFACEMETHODIMP OnDestroyUICommand(UINT32, UI_COMMANDTYPE, IUICommandHandler*) override { return S_OK; }
} g_app;

bool SavePng(HWND hwnd, const std::wstring& path, bool fromScreen = false) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);
    if (fromScreen || !PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)) BitBlt(dc, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(dc, old);
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    bool ok;
    {
        Gdiplus::Bitmap image(bmp, nullptr);
        ok = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    printf("  screenshot %s: %ls\n", ok ? "saved" : "FAILED", path.c_str());
    return ok;
}

IUIAutomationElement* FindByName(IUIAutomation* uia, IUIAutomationElement* root, const wchar_t* name) {
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(name);
    IUIAutomationCondition* cond = nullptr;
    IUIAutomationElement* found = nullptr;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, v, &cond))) {
        root->FindFirst(TreeScope_Descendants, cond, &found);
        cond->Release();
    }
    VariantClear(&v);
    return found;
}

bool ActivateTab(IUIAutomationElement* tab) {
    IUIAutomationSelectionItemPattern* sel = nullptr;
    if (SUCCEEDED(tab->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sel))) && sel) {
        HRESULT hr = sel->Select();
        sel->Release();
        printf("  tab selected through SelectionItem: 0x%08lX\n", hr);
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationLegacyIAccessiblePattern* acc = nullptr;
    if (SUCCEEDED(tab->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc))) && acc) {
        HRESULT hr = acc->DoDefaultAction();
        acc->Release();
        printf("  tab activated through DoDefaultAction: 0x%08lX\n", hr);
        if (SUCCEEDED(hr)) return true;
    }
    RECT r;
    if (FAILED(tab->get_CurrentBoundingRectangle(&r))) return false;
    POINT pt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    HWND target = WindowFromPoint(pt);
    ScreenToClient(target, &pt);
    PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
    PostMessageW(target, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    printf("  tab clicked through posted mouse messages\n");
    return true;
}

struct ProbeArgs {
    HWND hwnd;
    bool patched;
    std::wstring png;
    bool tabFound = false;
    bool contentFound = false;
};

DWORD WINAPI Probe(void* param) {
    auto a = (ProbeArgs*)param;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Sleep(1500);
    SavePng(a->hwnd, a->png + L"-before.png");

    IUIAutomation* uia = nullptr;
    IUIAutomationElement* root = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))
        && SUCCEEDED(uia->ElementFromHandle(a->hwnd, &root))) {
        IUIAutomationElement* tab = FindByName(uia, root, kOwn[0].label);
        a->tabFound = tab != nullptr;
        if (tab) {
            CONTROLTYPEID type = 0;
            tab->get_CurrentControlType(&type);
            printf("  UIA: element \"%ls\" found, control type %d\n", kOwn[0].label, type);
            if (ActivateTab(tab)) {
                Sleep(1000);
                SavePng(a->hwnd, a->png + L"-after.png");
                for (int i = 2; i < ARRAYSIZE(kOwn); i++) {
                    IUIAutomationElement* item = FindByName(uia, root, kOwn[i].label);
                    printf("  UIA: control \"%ls\" %s\n", kOwn[i].label, item ? "found" : "missing");
                    if (item) { a->contentFound = true; item->Release(); }
                }
            }
            tab->Release();
        } else {
            printf("  UIA: no element named \"%ls\"\n", kOwn[0].label);
        }
        root->Release();
        uia->Release();
    } else {
        printf("  UIA unavailable\n");
    }
    PostMessageW(a->hwnd, WM_CLOSE, 0, 0);
    CoUninitialize();
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HRESULT hr = CoCreateInstance(__uuidof(UIRibbonFramework), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_fw));
        if (FAILED(hr)) { printf("  CoCreateInstance(UIRibbonFramework) 0x%08lX\n", hr); return -1; }
        InstallResourceHooks();
        hr = g_fw->Initialize(hwnd, &g_app);
        if (FAILED(hr)) { printf("  IUIFramework::Initialize 0x%08lX\n", hr); return -1; }
        g_loadHr = g_fw->LoadUI(g_frame, kResName);
        printf("  LoadUI 0x%08lX, resource hooks: find %d, size %d, load %d, lock %d\n",
            g_loadHr, g_hitFind, g_hitSize, g_hitLoad, g_hitLock);
        for (auto& c : kOwn) g_fw->InvalidateUICommand(c.id, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_Label);
        SetTimer(hwnd, 1, 20000, nullptr);
        return 0;
    }
    case WM_TIMER:
        printf("  timeout\n");
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_fw) { g_fw->Destroy(); g_fw->Release(); g_fw = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Show(bool patched, bool full, const std::wstring& png) {
    g_frame = LoadFrame(full);
    std::vector<BYTE> original;
    if (!g_frame || !ReadResource(g_frame, original)) { Report(false, "read ribbon resource"); return g_failures; }
    if (patched) {
        std::string error;
        RibbonPatch::NewTab tab;
        bool ok = RibbonPatch::AddTab(original.data(), original.size(), tab, g_blob, &error);
        Report(ok, "build patched blob");
        if (!ok) { printf("  %s\n", error.c_str()); return g_failures; }
        g_serve = true;
    }

    OleInitialize(nullptr);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RibbonTestWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, patched ? L"RibbonTest - patched" : L"RibbonTest - original",
        WS_OVERLAPPEDWINDOW, 80, 80, 1100, 420, nullptr, nullptr, wc.hInstance, nullptr);
    Report(hwnd != nullptr, "create window with ribbon framework");
    Report(SUCCEEDED(g_loadHr), "IUIFramework::LoadUI");
    if (patched) Report(g_hitLock > 0, "patched blob served through UIRibbon imports");

    ProbeArgs probe = { hwnd, patched, png };
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        CloseHandle(CreateThread(nullptr, 0, Probe, &probe, 0, nullptr));
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    printf("  ribbon height %u, commands created %zu\n", g_ribbonHeight, g_created.size());
    for (auto& c : kOwn) {
        std::string keys;
        for (auto& k : g_updates[c.id]) keys += (keys.empty() ? "" : ",") + k;
        printf("  0x%04X created %s, properties queried: %s\n", c.id, g_created.count(c.id) ? "yes" : "no", keys.empty() ? "-" : keys.c_str());
    }
    if (patched) {
        Report(g_created.count(kOwn[0].id) > 0, "framework created the new tab command");
        Report(probe.tabFound, "new tab visible through UI Automation");
        Report(probe.contentFound, "new group controls visible after selecting the tab");
    }
    Gdiplus::GdiplusShutdown(gdip);
    OleUninitialize();
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}

int ReadToggle(IUIAutomationElement* e) {
    IUIAutomationTogglePattern* t = nullptr;
    if (FAILED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&t))) || !t) return -1;
    ToggleState state = ToggleState_Indeterminate;
    t->get_CurrentToggleState(&state);
    t->Release();
    return state;
}

bool Toggle(IUIAutomationElement* e) {
    IUIAutomationTogglePattern* t = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&t))) && t) {
        HRESULT hr = t->Toggle();
        t->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationLegacyIAccessiblePattern* acc = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc))) && acc) {
        HRESULT hr = acc->DoDefaultAction();
        acc->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

// Checks every open Explorer window from outside its process.
int Probe(const std::wstring& png, bool toggle) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
        Report(false, "create UI Automation client");
        return g_failures;
    }
    std::vector<HWND> windows;
    for (HWND hwnd = nullptr; (hwnd = FindWindowExW(nullptr, hwnd, L"CabinetWClass", nullptr)) != nullptr;)
        windows.push_back(hwnd);
    int index = 0;
    for (HWND hwnd : windows) {
        index++;
        wchar_t title[260] = L"";
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        printf("window %d \"%ls\" pid %lu\n", index, title, pid);
        IUIAutomationElement* root = nullptr;
        if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) { Report(false, "window element"); continue; }
        wchar_t suffix[16];
        swprintf_s(suffix, L"-%d.png", index);
        IUIAutomationElement* tab = FindByName(uia, root, kOwn[0].label);
        Report(tab != nullptr, "tab ExplorerTint present");
        if (tab) {
            SetForegroundWindow(hwnd);
            Sleep(300);
            ActivateTab(tab);
            Sleep(800);
            bool all = true;
            for (int i = 2; i < ARRAYSIZE(kOwn); i++) {
                IUIAutomationElement* item = FindByName(uia, root, kOwn[i].label);
                if (!item) { printf("  control \"%ls\" missing\n", kOwn[i].label); all = false; continue; }
                item->Release();
            }
            Report(all, "group controls present after selecting the tab");
            SavePng(hwnd, png + suffix, true);
            IUIAutomationElement* box = toggle ? FindByName(uia, root, kOwn[2].label) : nullptr;
            if (box) {
                int before = ReadToggle(box);
                bool ok = Toggle(box);
                box->Release();
                Sleep(500);
                ActivateTab(tab);
                Sleep(800);
                int after = -1;
                if ((box = FindByName(uia, root, kOwn[2].label)) != nullptr) {
                    after = ReadToggle(box);
                    box->Release();
                }
                printf("  toggle \"%ls\": %d -> %d\n", kOwn[2].label, before, after);
                Report(ok && before >= 0 && after >= 0 && before != after, "checkbox toggles");
                PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            }
            tab->Release();
        } else {
            SavePng(hwnd, png + suffix);
        }
        root->Release();
    }
    Report(index > 0, "at least one Explorer window");
    uia->Release();
    Gdiplus::GdiplusShutdown(gdip);
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}

}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* mode = argc > 1 ? argv[1] : L"--check";
    const wchar_t* file = nullptr;
    const wchar_t* out = nullptr;
    bool full = false;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring png = std::wstring(temp) + L"RibbonTest";
    for (int i = 2; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--file") && i + 1 < argc) file = argv[++i];
        else if (!_wcsicmp(argv[i], L"--out") && i + 1 < argc) out = argv[++i];
        else if (!_wcsicmp(argv[i], L"--png") && i + 1 < argc) png = argv[++i];
        else if (!_wcsicmp(argv[i], L"--full")) full = true;
    }
    if (!_wcsicmp(mode, L"--check")) return Check(file, out);
    if (!_wcsicmp(mode, L"--probe")) {
        bool toggle = false;
        for (int i = 2; i < argc; i++)
            if (!_wcsicmp(argv[i], L"--toggle")) toggle = true;
        return Probe(png + L"-explorer", toggle);
    }
    if (!_wcsicmp(mode, L"--show")) {
        bool patched = argc > 2 && !_wcsicmp(argv[2], L"patched");
        return Show(patched, full, png + (patched ? L"-patched" : L"-original"));
    }
    printf("usage: RibbonTest --check [--file blob] [--out patched.bin]\n"
           "       RibbonTest --show original|patched [--full] [--png prefix]\n"
           "       RibbonTest --probe [--toggle] [--png prefix]\n");
    return 2;
}
