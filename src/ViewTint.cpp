#include "ViewTint.h"
#include "FolderHooks.h"
#include "GitStatus.h"
#include "ImportPatch.h"
#include "OrderColumn.h"
#include "Settings.h"
#include <commctrl.h>
#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <vssym32.h>

namespace {

const UINT_PTR kSubclassId = 0x54494E54;
const COLORREF kDarkBack = RGB(0x20, 0x20, 0x20);
const COLORREF kDarkText = RGB(0xFF, 0xFF, 0xFF);
const COLORREF kDarkHeaderText = RGB(0xDE, 0xDE, 0xDE);

struct ViewTint {
    HWND defView = nullptr;
    HWND list = nullptr;
    IFolderView* folderView = nullptr;
    IShellFolder* folder = nullptr;
    Settings settings;
    std::wstring path;
    std::wstring repo;
    std::wstring prefix;  // folder path relative to the repository root, with a trailing backslash
    bool dark = false;
    HWND header = nullptr;
    COLORREF headerText = kDarkHeaderText;
    COLORREF textColor = 0;
    COLORREF backColor = 0;
    int cachedIndex = -1;
    const StateStyle* cachedStyle = nullptr;
    OrderColumn* order = nullptr;
};

bool IsLight(COLORREF c) {
    return GetRValue(c) + GetGValue(c) + GetBValue(c) >= 3 * 128;
}

std::wstring FolderPath(IFolderView* fv) {
    std::wstring path;
    IPersistFolder2* pf = nullptr;
    if (!fv || FAILED(fv->GetFolder(IID_PPV_ARGS(&pf)))) return path;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(pf->GetCurFolder(&pidl))) {
        wchar_t buf[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, buf)) path = buf;
        CoTaskMemFree(pidl);
    }
    pf->Release();
    return path;
}

bool ItemName(ViewTint* t, PCUITEMID_CHILD child, std::wstring& name) {
    STRRET sr;
    wchar_t buf[MAX_PATH];
    if (FAILED(t->folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr)) ||
        FAILED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
        return false;
    name = buf;
    return true;
}

const StateStyle* GitStyle(ViewTint* t, PCUITEMID_CHILD child) {
    std::wstring name;
    if (!t->settings.git || t->repo.empty() || !ItemName(t, child, name)) return nullptr;
    switch (GitStatus::Query(t->repo, t->prefix + name)) {
    case GitState::Staged:    return &t->settings.staged;
    case GitState::Untracked: return &t->settings.untracked;
    case GitState::Ignored:   return &t->settings.ignored;
    default:                  return nullptr;
    }
}

const StateStyle* StyleFor(ViewTint* t, int index) {
    if (index == t->cachedIndex) return t->cachedStyle;
    const StateStyle* style = nullptr;
    PITEMID_CHILD child = nullptr;
    if (t->folderView && t->folder && SUCCEEDED(t->folderView->Item(index, &child))) {
        DWORD attrs;
        if (ItemAttributes(t->folder, child, attrs) && IsSystemItem(attrs)) style = &t->settings.system;
        else style = GitStyle(t, child);
        CoTaskMemFree(child);
    }
    t->cachedIndex = index;
    t->cachedStyle = style;
    return style;
}

COLORREF ListBack(HWND list) {
    COLORREF c = ListView_GetBkColor(list);
    return c == CLR_NONE ? GetSysColor(COLOR_WINDOW) : c;
}

void FillSolid(HDC hdc, const RECT& rc, COLORREF c) {
    COLORREF old = SetDCBrushColor(hdc, c);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    SetDCBrushColor(hdc, old);
}

LRESULT OnCustomDraw(ViewTint* t, NMLVCUSTOMDRAW* cd, LRESULT r) {
    if (r & CDRF_SKIPDEFAULT) return r;
    DWORD stage = cd->nmcd.dwDrawStage;
    if (stage == CDDS_PREPAINT) {
        t->cachedIndex = -1;
        t->backColor = ListBack(t->list);
        t->textColor = ListView_GetTextColor(t->list);
        if (t->dark && !IsLight(t->textColor)) t->textColor = kDarkText;
        return r | CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    if (stage == CDDS_POSTPAINT) {
        OrderColumnPostPaint(t->order, cd->nmcd.hdc);
        return r;
    }
    if ((stage & ~CDDS_SUBITEM) != CDDS_ITEMPREPAINT) return r;

    bool sub = (stage & CDDS_SUBITEM) != 0;
    int index = (int)cd->nmcd.dwItemSpec;
    const StateStyle* st = StyleFor(t, index);

    COLORREF bg = t->backColor;
    if (st && st->back.set) {
        bg = Blend(st->back, t->backColor);
        if (!sub) {
            RECT rc = cd->nmcd.rc;
            if (IsRectEmpty(&rc)) ListView_GetItemRect(t->list, index, &rc, LVIR_BOUNDS);
            FillSolid(cd->nmcd.hdc, rc, bg);
        }
        if (!(cd->nmcd.uItemState & CDIS_SELECTED)) cd->clrTextBk = bg;
    }

    bool wholeRow = st == &t->settings.system;
    bool nameColumn = !sub || cd->iSubItem == 0;
    if (st && st->text.set && (nameColumn || wholeRow)) cd->clrText = Blend(st->text, bg);
    else if (sub || (t->dark && !IsLight(cd->clrText))) cd->clrText = t->textColor;

    r |= CDRF_NEWFONT;
    if (st && !sub) r |= CDRF_NOTIFYSUBITEMDRAW;
    return r;
}

COLORREF HeaderTextColor(HWND header) {
    COLORREF c = kDarkHeaderText;
    if (HTHEME th = OpenThemeData(header, L"Header")) {
        COLORREF themed;
        if (SUCCEEDED(GetThemeColor(th, HP_HEADERITEM, 0, TMT_TEXTCOLOR, &themed)) && IsLight(themed)) c = themed;
        CloseThemeData(th);
    }
    return c;
}

void DarkenHeader(HWND header) {
    AllowDarkMode(header);
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
}

void DarkenList(HWND list) {
    AllowDarkMode(list);
    SetWindowTheme(list, L"DarkMode_ItemsView", nullptr);
    if (HWND header = ListView_GetHeader(list)) DarkenHeader(header);
    if (IsLight(ListBack(list))) {
        ListView_SetBkColor(list, kDarkBack);
        ListView_SetTextBkColor(list, kDarkBack);
    }
    if (!IsLight(ListView_GetTextColor(list))) ListView_SetTextColor(list, kDarkText);
}

void ApplyDarkList(ViewTint* t) {
    Log(L"view: list back=%06X text=%06X", ListBack(t->list), ListView_GetTextColor(t->list));
    t->dark = IsDarkMode();
    if (!t->dark) return;
    DarkenList(t->list);
    if (HWND header = ListView_GetHeader(t->list)) {
        t->header = header;
        t->headerText = HeaderTextColor(header);
    }
}

bool HasClass(HWND hwnd, const wchar_t* cls) {
    wchar_t name[64];
    return hwnd && GetClassNameW(hwnd, name, ARRAYSIZE(name)) && wcscmp(name, cls) == 0;
}

LRESULT CALLBACK EarlyDefViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillSolid((HDC)wp, rc, kDarkBack);
        return 1;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EarlyDefViewProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ref: 0 for the list, 1 for its header.
LRESULT CALLBACK EarlyCreateProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_CREATE) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        ref ? DarkenHeader(hwnd) : DarkenList(hwnd);
        RemoveWindowSubclass(hwnd, EarlyCreateProc, id);
        return r;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EarlyCreateProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool IsShellList(HWND hwnd) {
    return HasClass(hwnd, WC_LISTVIEWW) && HasClass(GetParent(hwnd), L"SHELLDLL_DefView");
}

thread_local HHOOK t_cbtHook = nullptr;
thread_local int t_cbtRefs = 0;

// Darkens shell views as they are created, before their first paint.
LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND && IsDarkMode()) {
        HWND hwnd = (HWND)wp;
        HWND parent = ((CBT_CREATEWNDW*)lp)->lpcs->hwndParent;
        if (HasClass(hwnd, L"SHELLDLL_DefView"))
            SetWindowSubclass(hwnd, EarlyDefViewProc, kSubclassId + 1, 0);
        else if (HasClass(hwnd, WC_LISTVIEWW) && HasClass(parent, L"SHELLDLL_DefView"))
            SetWindowSubclass(hwnd, EarlyCreateProc, kSubclassId + 1, 0);
        else if (HasClass(hwnd, WC_HEADERW) && IsShellList(parent))
            SetWindowSubclass(hwnd, EarlyCreateProc, kSubclassId + 1, 1);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

typedef HTHEME (WINAPI* OpenNcThemeData_t)(HWND, LPCWSTR);
OpenNcThemeData_t s_openNcThemeData;

// comctl32 opens the list scrollbar theme itself, ignoring the window theme set on the list.
HTHEME WINAPI HookOpenNcThemeData(HWND hwnd, LPCWSTR classList) {
    if (classList && wcscmp(classList, L"ScrollBar") == 0 && IsShellList(hwnd) && IsDarkMode())
        return s_openNcThemeData(nullptr, L"DarkMode_Explorer::ScrollBar");
    return s_openNcThemeData(hwnd, classList);
}

BOOL CALLBACK InstallScrollBarHook(PINIT_ONCE, void*, void**) {
    s_openNcThemeData = (OpenNcThemeData_t)GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(49));
    if (!s_openNcThemeData) return TRUE;
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return TRUE;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < ARRAYSIZE(mods); i++) {
        wchar_t name[MAX_PATH];
        if (!K32GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, ARRAYSIZE(name)) ||
            _wcsicmp(name, L"comctl32.dll") != 0)
            continue;
        int n = PatchImports(mods[i], "uxtheme.dll", MAKEINTRESOURCEA(49), (void*)HookOpenNcThemeData);
        Log(L"view: %d OpenNcThemeData slot(s) patched in comctl32 %p", n, mods[i]);
    }
    return TRUE;
}

INIT_ONCE s_scrollBarOnce = INIT_ONCE_STATIC_INIT;

void ComputeRepo(ViewTint* t) {
    t->repo.clear();
    t->prefix.clear();
    if (!t->settings.git || t->path.empty()) return;
    GitStatus::SetGitPath(t->settings.gitPath);
    t->repo = GitStatus::RepoFor(t->path);
    if (t->repo.empty()) return;
    std::wstring lower = Lower(t->path);
    size_t skip = t->repo.size() + (t->repo.back() == L'\\' ? 0 : 1);
    if (lower.size() > skip) t->prefix = lower.substr(skip) + L"\\";
}

void RefreshView(ViewTint* t) {
    IShellView* view = nullptr;
    if (t->folderView && SUCCEEDED(t->folderView->QueryInterface(IID_PPV_ARGS(&view)))) {
        view->Refresh();
        view->Release();
    }
}

void OnSettingsChanged(ViewTint* t) {
    Settings old = t->settings;
    t->settings = *CurrentSettings();
    t->cachedIndex = -1;
    t->cachedStyle = nullptr;
    if (old.git != t->settings.git || old.gitPath != t->settings.gitPath) ComputeRepo(t);
    if (old.systemLast != t->settings.systemLast || old.hideNames != t->settings.hideNames ||
        old.hiddenNames != t->settings.hiddenNames || old.repoIcon != t->settings.repoIcon || old.repoBadge != t->settings.repoBadge ||
        old.folderIcons != t->settings.folderIcons || old.iconRoots != t->settings.iconRoots)
        RefreshView(t);
    InvalidateRect(t->list, nullptr, FALSE);
}

void Destroy(ViewTint* t) {
    if (t->folder) t->folder->Release();
    if (t->folderView) t->folderView->Release();
    delete t;
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto t = (ViewTint*)ref;
    LRESULT result;
    if (OrderColumnListMessage(t->order, hwnd, msg, wp, lp, result)) return result;
    if (msg == WM_NOTIFY && t->dark) {
        auto hdr = (NMHDR*)lp;
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == ListView_GetHeader(hwnd)) {
            if (hdr->hwndFrom != t->header) {
                t->header = hdr->hwndFrom;
                DarkenHeader(t->header);
                t->headerText = HeaderTextColor(t->header);
            }
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            auto cd = (NMCUSTOMDRAW*)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return r | CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) SetTextColor(cd->hdc, t->headerText);
            return r;
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ListSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto t = (ViewTint*)ref;
    if (msg == WM_NOTIFY) {
        auto hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == t->list && hdr->code == NM_CUSTOMDRAW) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            return OnCustomDraw(t, (NMLVCUSTOMDRAW*)lp, r);
        }
        if (hdr->hwndFrom == t->list && hdr->code == LVN_GETDISPINFOW && t->order) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            OrderColumnDispInfo(t->order, (NMLVDISPINFOW*)lp);
            return r;
        }
    } else if (msg == GitStatus::RefreshMessage()) {
        InvalidateRect(t->list, nullptr, FALSE);
        return 0;
    } else if (msg == SettingsChangedMessage()) {
        OnSettingsChanged(t);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        GitStatus::RemoveListener(hwnd);
        RemoveSettingsListener(hwnd);
        UnregisterSortView(t->folder);
        DestroyOrderColumn(t->order);
        t->order = nullptr;
        RemoveWindowSubclass(hwnd, SubclassProc, id);
        Destroy(t);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}

void AttachViewTint(IShellView* view) {
    HWND defView = nullptr;
    if (FAILED(view->GetWindow(&defView)) || !defView) return;
    if (GetWindowSubclass(defView, SubclassProc, kSubclassId, nullptr)) return;
    HWND list = FindWindowExW(defView, nullptr, WC_LISTVIEWW, nullptr);
    if (!list) {
        Log(L"view: no SysListView32 under DefView %p, items view still active", defView);
        return;
    }

    auto t = new ViewTint;
    t->defView = defView;
    t->list = list;
    t->settings = *CurrentSettings();
    view->QueryInterface(IID_PPV_ARGS(&t->folderView));
    if (t->folderView) t->folderView->GetFolder(IID_PPV_ARGS(&t->folder));
    RegisterSortView(t->folder, t->folderView);

    std::wstring path = FolderPath(t->folderView);
    t->path = path;
    ComputeRepo(t);
    Log(L"view: attached to %s (repo \"%s\")", path.c_str(), t->repo.c_str());

    ApplyDarkList(t);
    SetWindowSubclass(list, ListSubclassProc, kSubclassId, (DWORD_PTR)t);
    SetWindowSubclass(defView, SubclassProc, kSubclassId, (DWORD_PTR)t);
    t->order = CreateOrderColumn(list, t->folderView, path);
    GitStatus::AddListener(defView);
    AddSettingsListener(defView);
    InvalidateRect(list, nullptr, TRUE);
}

void HookViewCreation(bool on) {
    if (on) {
        InitOnceExecuteOnce(&s_scrollBarOnce, InstallScrollBarHook, nullptr, nullptr);
        if (t_cbtRefs++ == 0)
            t_cbtHook = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());
    } else if (t_cbtRefs > 0 && --t_cbtRefs == 0 && t_cbtHook) {
        UnhookWindowsHookEx(t_cbtHook);
        t_cbtHook = nullptr;
    }
}
