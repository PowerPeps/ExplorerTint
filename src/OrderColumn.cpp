#include "OrderColumn.h"
#include "Order.h"
#include <algorithm>
#include <cwchar>
#include <unordered_set>
#include <vector>
#include <shlguid.h>
#include <shlobj.h>

class ReorderTarget;

struct OrderColumn {
    HWND list = nullptr;
    IFolderView2* view = nullptr;
    IShellFolder* folder = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    std::wstring path;
    int column = -2;  // list column of the host property, -1 when hidden, -2 when unknown
    int lineY = -1;
    HWND dropWindow = nullptr;
    ReorderTarget* target = nullptr;
};

namespace {

// Zero-width space keeps the label distinct from other "#" columns (music track number).
const wchar_t kLabel[] = L"#​";
const PROPERTYKEY kNameKey = { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 10 };

UINT ResortMessage() {
    static UINT msg = RegisterWindowMessageW(L"ExplorerTint.Resort");
    return msg;
}

bool ItemData(OrderColumn* oc, int index, WIN32_FIND_DATAW& fd) {
    PITEMID_CHILD child = nullptr;
    if (FAILED(oc->view->Item(index, &child))) return false;
    bool ok = SUCCEEDED(SHGetDataFromIDListW(oc->folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)));
    CoTaskMemFree(child);
    return ok;
}

int HostColumn(OrderColumn* oc) {
    if (oc->column != -2) return oc->column;
    oc->column = -1;
    HWND header = ListView_GetHeader(oc->list);
    int count = header ? Header_GetItemCount(header) : 0;
    for (int i = 0; i < count; i++) {
        wchar_t text[128] = L"";
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT;
        col.pszText = text;
        col.cchTextMax = ARRAYSIZE(text);
        if (ListView_GetColumn(oc->list, i, &col) && (wcscmp(text, kLabel) == 0 || Order::HostLabel() == text)) {
            oc->column = i;
            break;
        }
    }
    return oc->column;
}

void RenameExisting(OrderColumn* oc) {
    oc->column = -2;
    int i = HostColumn(oc);
    if (i < 0) return;
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT;
    col.pszText = (LPWSTR)kLabel;
    ListView_SetColumn(oc->list, i, &col);
}

void EnsureFirst(OrderColumn* oc) {
    IColumnManager* cm = nullptr;
    if (FAILED(oc->view->QueryInterface(IID_PPV_ARGS(&cm)))) return;
    const PROPERTYKEY& host = Order::HostKey();
    UINT n = 0;
    if (SUCCEEDED(cm->GetColumnCount(CM_ENUM_VISIBLE, &n))) {
        std::vector<PROPERTYKEY> keys(n);
        if (n == 0 || SUCCEEDED(cm->GetColumns(CM_ENUM_VISIBLE, keys.data(), n))) {
            if (n == 0 || !IsEqualPropertyKey(keys[0], host)) {
                bool present = false;
                std::vector<PROPERTYKEY> wanted{ host };
                for (auto& k : keys) {
                    if (IsEqualPropertyKey(k, host)) present = true;
                    else wanted.push_back(k);
                }
                HRESULT hr = cm->SetColumns(wanted.data(), (UINT)wanted.size());
                if (SUCCEEDED(hr) && !present) {
                    CM_COLUMNINFO info = { sizeof(info) };
                    info.dwMask = CM_MASK_WIDTH;
                    info.uWidth = 44;
                    cm->SetColumnInfo(host, &info);
                }
                Log(L"order: \"#\" column placed first in %s (%08X)", oc->path.c_str(), hr);
            }
        }
    }
    cm->Release();
    RenameExisting(oc);
}

bool SortedByHost(OrderColumn* oc, bool& descending) {
    SORTCOLUMN sc;
    if (FAILED(oc->view->GetSortColumns(&sc, 1)) || !IsEqualPropertyKey(sc.propkey, Order::HostKey())) return false;
    descending = sc.direction == SORT_DESCENDING;
    return true;
}

void Resort(OrderColumn* oc) {
    bool descending = false;
    if (SortedByHost(oc, descending)) {
        SORTCOLUMN byName = { kNameKey, SORT_ASCENDING };
        oc->view->SetSortColumns(&byName, 1);
    }
    SORTCOLUMN byRank = { Order::HostKey(), descending ? SORT_DESCENDING : SORT_ASCENDING };
    oc->view->SetSortColumns(&byRank, 1);
}

bool ReorderAllowed(OrderColumn* oc) {
    if (ListView_GetView(oc->list) != LV_VIEW_DETAILS) return false;
    bool descending;
    return !Order::Has(oc->path) || SortedByHost(oc, descending);
}

// Insertion index under a screen point, or false when the drop belongs to DefView (into a folder).
bool InsertAt(OrderColumn* oc, POINTL screen, int& insert) {
    POINT p = { screen.x, screen.y };
    ScreenToClient(oc->list, &p);
    RECT header = {};
    if (HWND h = ListView_GetHeader(oc->list)) {
        GetWindowRect(h, &header);
        MapWindowPoints(nullptr, oc->list, (POINT*)&header, 2);
    }
    if (p.y < header.bottom) return false;

    int count = ListView_GetItemCount(oc->list);
    if (count == 0) {
        insert = 0;
        return true;
    }
    RECT first;
    ListView_GetItemRect(oc->list, 0, &first, LVIR_BOUNDS);
    LVHITTESTINFO hit = {};
    hit.pt = { first.left + 2, p.y };
    int i = ListView_HitTest(oc->list, &hit);
    if (i < 0) {
        RECT last;
        ListView_GetItemRect(oc->list, count - 1, &last, LVIR_BOUNDS);
        if (p.y < last.bottom) return false;
        insert = count;
        return true;
    }

    RECT rc;
    ListView_GetItemRect(oc->list, i, &rc, LVIR_BOUNDS);
    double frac = rc.bottom > rc.top ? double(p.y - rc.top) / (rc.bottom - rc.top) : 0;
    WIN32_FIND_DATAW fd;
    bool folder = ItemData(oc, i, fd) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (folder) {
        if (frac < 0.25) insert = i;
        else if (frac > 0.75) insert = i + 1;
        else return false;
    } else {
        insert = frac < 0.5 ? i : i + 1;
    }
    return true;
}

void InvalidateLine(OrderColumn* oc, int y) {
    if (y < 0) return;
    RECT rc;
    GetClientRect(oc->list, &rc);
    rc.top = y - 3;
    rc.bottom = y + 3;
    InvalidateRect(oc->list, &rc, FALSE);
}

void ShowLine(OrderColumn* oc, int insert) {
    int count = ListView_GetItemCount(oc->list);
    RECT rc = {};
    int y;
    if (insert < count) {
        ListView_GetItemRect(oc->list, insert, &rc, LVIR_BOUNDS);
        y = rc.top;
    } else if (count > 0) {
        ListView_GetItemRect(oc->list, count - 1, &rc, LVIR_BOUNDS);
        y = rc.bottom;
    } else {
        y = 0;
    }
    if (y == oc->lineY) return;
    InvalidateLine(oc, oc->lineY);
    oc->lineY = y;
    InvalidateLine(oc, y);
    UpdateWindow(oc->list);
}

void HideLine(OrderColumn* oc) {
    InvalidateLine(oc, oc->lineY);
    oc->lineY = -1;
    UpdateWindow(oc->list);
}

// Names of the dragged items when they all come from this folder.
std::vector<std::wstring> DraggedNames(OrderColumn* oc, IDataObject* data) {
    std::vector<std::wstring> names;
    static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    FORMATETC fmt = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    if (!data || FAILED(data->GetData(&fmt, &medium))) return names;
    if (auto cida = (CIDA*)GlobalLock(medium.hGlobal)) {
        auto parent = (PCIDLIST_ABSOLUTE)((BYTE*)cida + cida->aoffset[0]);
        if (ILIsEqual(parent, oc->pidl)) {
            for (UINT i = 1; i <= cida->cidl; i++) {
                auto child = (PCUITEMID_CHILD)((BYTE*)cida + cida->aoffset[i]);
                WIN32_FIND_DATAW fd;
                if (!ILIsChild(child) ||
                    FAILED(SHGetDataFromIDListW(oc->folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)))) {
                    names.clear();
                    break;
                }
                names.push_back(fd.cFileName);
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return names;
}

void Reorder(OrderColumn* oc, const std::vector<std::wstring>& dragged, int insert) {
    int n = 0;
    if (FAILED(oc->view->ItemCount(SVGIO_ALLVIEW, &n))) return;
    std::vector<std::wstring> shown;
    for (int i = 0; i < n; i++) {
        WIN32_FIND_DATAW fd;
        if (!ItemData(oc, i, fd)) return;
        shown.push_back(fd.cFileName);
    }

    std::unordered_set<std::wstring> moving;
    for (auto& name : dragged) moving.insert(Lower(name));
    std::vector<std::wstring> rest, moved;
    int pos = insert;
    for (int i = 0; i < n; i++) {
        if (moving.count(Lower(shown[i]))) {
            moved.push_back(shown[i]);
            if (i < insert) pos--;
        } else {
            rest.push_back(shown[i]);
        }
    }
    pos = std::max(0, std::min(pos, (int)rest.size()));
    rest.insert(rest.begin() + pos, moved.begin(), moved.end());
    if (rest == shown) return;

    bool descending = false;
    if (SortedByHost(oc, descending) && descending) std::reverse(rest.begin(), rest.end());
    Order::Set(oc->path, rest);
    Log(L"order: %s reordered (%d item(s))", oc->path.c_str(), (int)rest.size());
    PostMessageW(oc->list, ResortMessage(), 0, 0);
}

void FillLine(HDC hdc, const RECT& rc, COLORREF c) {
    COLORREF old = SetDCBrushColor(hdc, c);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    SetDCBrushColor(hdc, old);
}

}

// Wraps DefView's drop target: drops between rows of this folder reorder, everything else goes to DefView.
class ReorderTarget : public IDropTarget {
    LONG m_ref = 1;
    OrderColumn* m_oc;
    IDropTarget* m_inner;
    IDropTargetHelper* m_helper = nullptr;
    IDataObject* m_data = nullptr;
    std::vector<std::wstring> m_names;
    bool m_ours = false;
    bool m_innerIn = false;
    int m_insert = -1;

    void ReleaseData() {
        if (m_data) m_data->Release();
        m_data = nullptr;
        m_names.clear();
    }

    HRESULT Move(DWORD keys, POINTL pt, DWORD* effect) {
        int insert = -1;
        bool plain = !(keys & (MK_RBUTTON | MK_MBUTTON | MK_CONTROL | MK_SHIFT | MK_ALT));
        if (m_oc && plain && !m_names.empty() && ReorderAllowed(m_oc) && InsertAt(m_oc, pt, insert)) {
            if (m_innerIn) {
                m_inner->DragLeave();
                m_innerIn = false;
            }
            DWORD e = (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
            POINT p = { pt.x, pt.y };
            if (!m_ours) {
                if (m_helper) m_helper->DragEnter(m_oc->list, m_data, &p, e);
                m_ours = true;
            } else if (m_helper) {
                m_helper->DragOver(&p, e);
            }
            m_insert = insert;
            ShowLine(m_oc, insert);
            *effect = e;
            return S_OK;
        }
        if (m_ours) {
            if (m_helper) m_helper->DragLeave();
            if (m_oc) HideLine(m_oc);
            m_ours = false;
        }
        if (!m_innerIn) {
            m_innerIn = true;
            return m_inner->DragEnter(m_data, keys, pt, effect);
        }
        return m_inner->DragOver(keys, pt, effect);
    }

public:
    ReorderTarget(OrderColumn* oc, IDropTarget* inner) : m_oc(oc), m_inner(inner) {
        m_inner->AddRef();
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_helper));
    }
    ~ReorderTarget() {
        ReleaseData();
        if (m_helper) m_helper->Release();
        m_inner->Release();
    }
    void Detach() { m_oc = nullptr; }
    IDropTarget* Inner() { return m_inner; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
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

    IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        ReleaseData();
        m_ours = m_innerIn = false;
        m_data = data;
        if (m_data) m_data->AddRef();
        if (m_oc) m_names = DraggedNames(m_oc, data);
        return Move(keys, pt, effect);
    }
    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        return Move(keys, pt, effect);
    }
    IFACEMETHODIMP DragLeave() override {
        if (m_ours) {
            if (m_helper) m_helper->DragLeave();
            if (m_oc) HideLine(m_oc);
        } else if (m_innerIn) {
            m_inner->DragLeave();
        }
        m_ours = m_innerIn = false;
        ReleaseData();
        return S_OK;
    }
    IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        HRESULT hr = S_OK;
        if (m_ours) {
            POINT p = { pt.x, pt.y };
            if (m_helper) m_helper->Drop(data, &p, DROPEFFECT_NONE);
            *effect = DROPEFFECT_NONE;  // a move effect would let the source delete the files
            if (m_oc) {
                HideLine(m_oc);
                Reorder(m_oc, m_names, m_insert);
            }
        } else if (m_innerIn) {
            hr = m_inner->Drop(data, keys, pt, effect);
        } else {
            *effect = DROPEFFECT_NONE;
        }
        m_ours = m_innerIn = false;
        ReleaseData();
        return hr;
    }
};

namespace {

// The property is undocumented: only trust it when its vtable points into a loaded module.
bool LooksLikeInterface(void* p) {
    __try {
        void* first = **(void***)p;
        HMODULE mod;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)first, &mod) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HookDrop(OrderColumn* oc) {
    HWND candidates[] = { oc->list, GetParent(oc->list) };
    for (HWND hwnd : candidates) {
        auto prop = (IUnknown*)GetPropW(hwnd, L"OleDropTargetInterface");
        IDropTarget* inner = nullptr;
        if (!prop || !LooksLikeInterface(prop) || FAILED(prop->QueryInterface(IID_PPV_ARGS(&inner)))) continue;
        auto target = new ReorderTarget(oc, inner);
        inner->Release();
        RevokeDragDrop(hwnd);
        HRESULT hr = RegisterDragDrop(hwnd, target);
        if (FAILED(hr)) {
            RegisterDragDrop(hwnd, target->Inner());
            target->Release();
            Log(L"order: drop target wrap failed on %p (%08X)", hwnd, hr);
            return;
        }
        oc->target = target;
        oc->dropWindow = hwnd;
        return;
    }
    Log(L"order: no drop target found for list %p", oc->list);
}

}

OrderColumn* CreateOrderColumn(HWND list, IFolderView* view, const std::wstring& path) {
    if (!list || !view || path.empty()) return nullptr;
    auto oc = new OrderColumn;
    oc->list = list;
    oc->path = path;
    IPersistFolder2* pf = nullptr;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(&oc->view))) || FAILED(view->GetFolder(IID_PPV_ARGS(&oc->folder))) ||
        FAILED(oc->folder->QueryInterface(IID_PPV_ARGS(&pf))) || FAILED(pf->GetCurFolder(&oc->pidl))) {
        if (pf) pf->Release();
        DestroyOrderColumn(oc);
        return nullptr;
    }
    pf->Release();
    if (Order::Has(path)) EnsureFirst(oc);
    else RenameExisting(oc);
    HookDrop(oc);
    return oc;
}

void DestroyOrderColumn(OrderColumn* oc) {
    if (!oc) return;
    if (oc->target) {
        oc->target->Detach();
        oc->target->Release();
    }
    if (oc->view) oc->view->Release();
    if (oc->folder) oc->folder->Release();
    CoTaskMemFree(oc->pidl);
    delete oc;
}

bool OrderColumnListMessage(OrderColumn* oc, HWND list, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!oc) return false;
    if (msg == ResortMessage()) {
        EnsureFirst(oc);
        Resort(oc);
        InvalidateRect(list, nullptr, FALSE);
        result = 0;
        return true;
    }
    if (msg == LVM_DELETECOLUMN) {
        oc->column = -2;
        return false;
    }
    if (msg != LVM_INSERTCOLUMNW && msg != LVM_SETCOLUMNW) return false;
    oc->column = -2;
    auto col = (const LVCOLUMNW*)lp;
    if (!col || !(col->mask & LVCF_TEXT) || !col->pszText || col->pszText == LPSTR_TEXTCALLBACKW ||
        Order::HostLabel().empty() || Order::HostLabel() != col->pszText)
        return false;
    LVCOLUMNW copy = *col;
    copy.pszText = (LPWSTR)kLabel;
    result = DefSubclassProc(list, msg, wp, (LPARAM)&copy);
    return true;
}

void OrderColumnDispInfo(OrderColumn* oc, NMLVDISPINFOW* di) {
    if (!oc || !(di->item.mask & LVIF_TEXT) || !di->item.pszText || di->item.cchTextMax <= 0) return;
    int col = HostColumn(oc);
    if (col < 0 || di->item.iSubItem != col) return;
    WIN32_FIND_DATAW fd;
    int rank = ItemData(oc, di->item.iItem, fd) ? Order::Rank(oc->path, fd.cFileName) : 0;
    if (rank) swprintf_s(di->item.pszText, di->item.cchTextMax, L"%d", rank);
    else di->item.pszText[0] = 0;
}

void OrderColumnPostPaint(OrderColumn* oc, HDC hdc) {
    if (!oc || oc->lineY < 0) return;
    RECT rc;
    GetClientRect(oc->list, &rc);
    rc.top = oc->lineY - 1;
    rc.bottom = oc->lineY + 1;
    FillLine(hdc, rc, GetSysColor(COLOR_HIGHLIGHT));
}
