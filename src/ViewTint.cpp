#include "ViewTint.h"
#include "GitStatus.h"
#include "Settings.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

namespace {

const UINT_PTR kSubclassId = 0x54494E54;
const COLORREF kDarkBack = RGB(0x20, 0x20, 0x20);

struct ViewTint {
    HWND defView = nullptr;
    HWND list = nullptr;
    IFolderView* folderView = nullptr;
    IShellFolder* folder = nullptr;
    Settings settings;
    std::wstring repo;
    std::wstring prefix;  // folder path relative to the repository root, with a trailing backslash
    int cachedIndex = -1;
    const StateStyle* cachedStyle = nullptr;
};

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

LRESULT OnCustomDraw(ViewTint* t, NMLVCUSTOMDRAW* cd, LRESULT r) {
    if (r & CDRF_SKIPDEFAULT) return r;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        t->cachedIndex = -1;
        return r | CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const StateStyle* st = StyleFor(t, (int)cd->nmcd.dwItemSpec);
        if (!st) return r;
        COLORREF bg = ListBack(t->list);
        if (st->back.set && !(cd->nmcd.uItemState & CDIS_SELECTED)) {
            bg = Blend(st->back, bg);
            cd->clrTextBk = bg;
        }
        bool nameColumn = !(cd->nmcd.dwDrawStage & CDDS_SUBITEM) || cd->iSubItem == 0;
        if (st->text.set && nameColumn) cd->clrText = Blend(st->text, bg);
        r |= CDRF_NEWFONT;
        if (!(cd->nmcd.dwDrawStage & CDDS_SUBITEM)) r |= CDRF_NOTIFYSUBITEMDRAW;
        return r;
    }
    }
    return r;
}

bool IsDarkMode() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

void ApplyDarkList(HWND list) {
    COLORREF back = ListBack(list);
    Log(L"view: list back=%06X text=%06X", back, ListView_GetTextColor(list));
    if (!IsDarkMode() || GetRValue(back) + GetGValue(back) + GetBValue(back) < 3 * 128) return;
    SetWindowTheme(list, L"DarkMode_ItemsView", nullptr);
    if (HWND header = ListView_GetHeader(list)) SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
    ListView_SetBkColor(list, kDarkBack);
    ListView_SetTextBkColor(list, kDarkBack);
    ListView_SetTextColor(list, RGB(0xFF, 0xFF, 0xFF));
    Log(L"view: light list forced to dark");
}

void Destroy(ViewTint* t) {
    if (t->folder) t->folder->Release();
    if (t->folderView) t->folderView->Release();
    delete t;
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

    ApplyDarkList(list);
    SetWindowSubclass(defView, SubclassProc, kSubclassId, (DWORD_PTR)t);
    GitStatus::AddListener(defView);
}
