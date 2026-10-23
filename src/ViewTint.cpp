#include "ViewTint.h"
#include "GitStatus.h"
#include "Settings.h"
#include <commctrl.h>
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
    std::wstring repo;
    std::wstring prefix;  // folder path relative to the repository root, with a trailing backslash
    bool dark = false;
    COLORREF headerText = kDarkHeaderText;
    COLORREF textColor = 0;
    COLORREF backColor = 0;
    int cachedIndex = -1;
    const StateStyle* cachedStyle = nullptr;
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

bool ItemName(ViewTint* t, int index, std::wstring& name) {
    PITEMID_CHILD child = nullptr;
    if (!t->folderView || !t->folder || FAILED(t->folderView->Item(index, &child))) return false;
    STRRET sr;
    wchar_t buf[MAX_PATH];
    bool ok = SUCCEEDED(t->folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr)) &&
              SUCCEEDED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf)));
    CoTaskMemFree(child);
    if (ok) name = buf;
    return ok;
}

const StateStyle* StyleFor(ViewTint* t, int index) {
    if (index == t->cachedIndex) return t->cachedStyle;
    const StateStyle* style = nullptr;
    std::wstring name;
    if (!t->repo.empty() && ItemName(t, index, name)) {
        switch (GitStatus::Query(t->repo, t->prefix + name)) {
        case GitState::Staged:    style = &t->settings.staged; break;
        case GitState::Untracked: style = &t->settings.untracked; break;
        case GitState::Ignored:   style = &t->settings.ignored; break;
        default: break;
        }
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
        return r | CDRF_NOTIFYITEMDRAW;
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

    bool nameColumn = !sub || cd->iSubItem == 0;
    if (st && st->text.set && nameColumn) cd->clrText = Blend(st->text, bg);
    else if (sub || (t->dark && !IsLight(cd->clrText))) cd->clrText = t->textColor;

    r |= CDRF_NEWFONT;
    if (st && !sub) r |= CDRF_NOTIFYSUBITEMDRAW;
    return r;
}

bool IsDarkMode() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// uxtheme ordinal 133, undocumented, Windows 10 1809+.
void AllowDarkMode(HWND hwnd) {
    typedef BOOL (WINAPI* AllowDarkModeForWindow_t)(HWND, BOOL);
    static auto fn = (AllowDarkModeForWindow_t)GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(133));
    if (fn) fn(hwnd, TRUE);
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

void ApplyDarkList(ViewTint* t) {
    HWND list = t->list;
    COLORREF back = ListBack(list);
    COLORREF text = ListView_GetTextColor(list);
    Log(L"view: list back=%06X text=%06X", back, text);
    t->dark = IsDarkMode();
    if (!t->dark) return;

    AllowDarkMode(list);
    SetWindowTheme(list, L"ItemsView", nullptr);
    if (HWND header = ListView_GetHeader(list)) {
        AllowDarkMode(header);
        SetWindowTheme(header, L"ItemsView", nullptr);
        t->headerText = HeaderTextColor(header);
    }
    if (IsLight(back)) {
        ListView_SetBkColor(list, kDarkBack);
        ListView_SetTextBkColor(list, kDarkBack);
    }
    if (!IsLight(text)) ListView_SetTextColor(list, kDarkText);
}

void Destroy(ViewTint* t) {
    if (t->folder) t->folder->Release();
    if (t->folderView) t->folderView->Release();
    delete t;
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto t = (ViewTint*)ref;
    if (msg == WM_NOTIFY && t->dark) {
        auto hdr = (NMHDR*)lp;
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == ListView_GetHeader(hwnd)) {
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
    } else if (msg == GitStatus::RefreshMessage()) {
        InvalidateRect(t->list, nullptr, FALSE);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        GitStatus::RemoveListener(hwnd);
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
    t->settings = LoadSettings();
    view->QueryInterface(IID_PPV_ARGS(&t->folderView));
    if (t->folderView) t->folderView->GetFolder(IID_PPV_ARGS(&t->folder));

    std::wstring path = FolderPath(t->folderView);
    if (t->settings.git && !path.empty()) {
        GitStatus::SetGitPath(t->settings.gitPath);
        t->repo = GitStatus::RepoFor(path);
        if (!t->repo.empty()) {
            std::wstring lower = Lower(path);
            size_t skip = t->repo.size() + (t->repo.back() == L'\\' ? 0 : 1);
            if (lower.size() > skip) t->prefix = lower.substr(skip) + L"\\";
        }
    }
    Log(L"view: attached to %s (repo \"%s\")", path.c_str(), t->repo.c_str());

    ApplyDarkList(t);
    SetWindowSubclass(list, ListSubclassProc, kSubclassId, (DWORD_PTR)t);
    SetWindowSubclass(defView, SubclassProc, kSubclassId, (DWORD_PTR)t);
    GitStatus::AddListener(defView);
    InvalidateRect(list, nullptr, TRUE);
}
