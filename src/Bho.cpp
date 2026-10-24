#include "Bho.h"
#include "ClassicList.h"
#include "FolderHooks.h"
#include "RibbonTab.h"
#include "Settings.h"
#include "ViewTint.h"
#include <new>
#include <exdisp.h>
#include <exdispid.h>
#include <ocidl.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>

const CLSID CLSID_ExplorerTintBho =
    { 0xcdd9b530, 0x2936, 0x462d, { 0x89, 0x89, 0x0a, 0x70, 0x92, 0x6a, 0x4b, 0xb4 } };

static bool IsExplorerProcess() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return _wcsicmp(PathFindFileNameW(path), L"explorer.exe") == 0;
}

class Bho : public IObjectWithSite, public IDispatch {
    LONG m_ref = 1;
    IUnknown* m_site = nullptr;
    IShellBrowser* m_browser = nullptr;
    IWebBrowser2* m_web = nullptr;
    IConnectionPoint* m_events = nullptr;
    DWORD m_cookie = 0;
    bool m_hooked = false;

    void Disconnect() {
        if (m_hooked) {
            HookViewCreation(false);
            m_hooked = false;
        }
        if (m_events) {
            m_events->Unadvise(m_cookie);
            m_events->Release();
            m_events = nullptr;
        }
        if (m_web) { m_web->Release(); m_web = nullptr; }
        if (m_browser) { m_browser->Release(); m_browser = nullptr; }
        if (m_site) { m_site->Release(); m_site = nullptr; }
    }

    void OnNavigated() {
        IShellView* view = nullptr;
        if (m_browser && SUCCEEDED(m_browser->QueryActiveShellView(&view)) && view) {
            AttachViewTint(view);
            view->Release();
        }
    }

public:
    Bho() { InterlockedIncrement(&g_cRefModule); }
    ~Bho() {
        Disconnect();
        InterlockedDecrement(&g_cRefModule);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else if (riid == IID_IDispatch || riid == DIID_DWebBrowserEvents2) {
            *ppv = static_cast<IDispatch*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP SetSite(IUnknown* site) override {
        Disconnect();
        if (!site || !IsExplorerProcess()) return S_OK;
        m_site = site;
        m_site->AddRef();

        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&sp)))) {
            sp->QueryService(SID_SShellBrowser, IID_PPV_ARGS(&m_browser));
            sp->QueryService(SID_SWebBrowserApp, IID_PPV_ARGS(&m_web));
            sp->Release();
        }
        WatchSettings();
        auto settings = CurrentSettings();
        InstallClassicList(settings->classicList);
        InstallFolderHooks();
        InstallRibbonTab(settings->ribbonTab);
        HookViewCreation(true);
        m_hooked = true;

        IConnectionPointContainer* cpc = nullptr;
        if (m_web && SUCCEEDED(m_web->QueryInterface(IID_PPV_ARGS(&cpc)))) {
            if (SUCCEEDED(cpc->FindConnectionPoint(DIID_DWebBrowserEvents2, &m_events)) &&
                FAILED(m_events->Advise(static_cast<IDispatch*>(this), &m_cookie))) {
                m_events->Release();
                m_events = nullptr;
            }
            cpc->Release();
        }
        Log(L"bho: site set (browser %p, events %p)", m_browser, m_events);
        OnNavigated();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!m_site) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, ppv);
    }

    IFACEMETHODIMP GetTypeInfoCount(UINT* count) override {
        *count = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE) OnNavigated();
        else if (id == DISPID_ONQUIT) Disconnect();
        return S_OK;
    }
};

IUnknown* CreateBho() {
    return static_cast<IObjectWithSite*>(new (std::nothrow) Bho());
}
