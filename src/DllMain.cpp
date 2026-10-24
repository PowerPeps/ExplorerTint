#include "Common.h"
#include "Bho.h"
#include <new>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "propsys.lib")

HINSTANCE g_hinst = nullptr;
LONG g_cRefModule = 0;

static const wchar_t kName[] = L"ExplorerTint";
static const wchar_t kBhoKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Browser Helper Objects\\";
static const wchar_t kPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Ext\\CLSID";

class ClassFactory : public IClassFactory {
    LONG m_ref = 1;
public:
    ClassFactory() { InterlockedIncrement(&g_cRefModule); }
    ~ClassFactory() { InterlockedDecrement(&g_cRefModule); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (pOuter) return CLASS_E_NOAGGREGATION;
        IUnknown* p = CreateBho();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL f) override {
        f ? InterlockedIncrement(&g_cRefModule) : InterlockedDecrement(&g_cRefModule);
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    *ppv = nullptr;
    if (rclsid != CLSID_ExplorerTintBho) return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* cf = new (std::nothrow) ClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

// Import patches and worker threads stay live for the life of the process.
STDAPI DllCanUnloadNow() {
    return S_FALSE;
}

static std::wstring ClsidString() {
    wchar_t buf[64];
    StringFromGUID2(CLSID_ExplorerTintBho, buf, ARRAYSIZE(buf));
    return buf;
}

static LONG SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

STDAPI DllRegisterServer() {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(g_hinst, module, ARRAYSIZE(module));
    std::wstring clsid = ClsidString();
    std::wstring clsidKey = L"SOFTWARE\\Classes\\CLSID\\" + clsid;

    LONG e = SetString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, kName);
    if (!e) e = SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, module);
    if (!e) e = SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (!e) e = SetString(HKEY_LOCAL_MACHINE, kBhoKey + clsid, nullptr, kName);
    if (!e) {
        DWORD one = 1;
        e = RegSetKeyValueW(HKEY_LOCAL_MACHINE, (kBhoKey + clsid).c_str(), L"NoInternetExplorer", REG_DWORD,
                            &one, sizeof(one));
    }
    if (!e) e = SetString(HKEY_LOCAL_MACHINE, kPolicyKey, clsid.c_str(), L"1");
    return e ? HRESULT_FROM_WIN32(e) : S_OK;
}

STDAPI DllUnregisterServer() {
    std::wstring clsid = ClsidString();
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (kBhoKey + clsid).c_str());
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kPolicyKey, clsid.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\Classes\\CLSID\\" + clsid).c_str());
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
