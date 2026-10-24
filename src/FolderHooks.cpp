#include "FolderHooks.h"
#include "FolderIcons.h"
#include "Order.h"
#include <shlobj.h>
#include <thumbcache.h>

namespace {

const int kCompareIDs = 7;
const int kGetAttributesOf = 9;
const int kGetIconOf = 3;
const SFGAOF kHiddenFlags = SFGAO_HIDDEN | SFGAO_GHOSTED;

typedef HRESULT (STDMETHODCALLTYPE* CompareIDs_t)(IShellFolder*, LPARAM, PCUIDLIST_RELATIVE, PCUIDLIST_RELATIVE);
typedef HRESULT (STDMETHODCALLTYPE* GetAttributesOf_t)(IShellFolder*, UINT, PCUITEMID_CHILD_ARRAY, SFGAOF*);
typedef HRESULT (STDMETHODCALLTYPE* GetIconOf_t)(IShellIcon*, PCUITEMID_CHILD, UINT, int*);

struct Patched {
    void** vtable;
    CompareIDs_t compare;
    GetAttributesOf_t attributes;
};

Patched s_patched[4];
volatile LONG s_patchedCount;

struct IconPatched {
    void** vtable;
    GetIconOf_t iconOf;
};

IconPatched s_icons[4];
volatile LONG s_iconCount;
INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
thread_local bool t_inAttributes;

struct SortView {
    IShellFolder* folder;
    IFolderView2* view;
    DWORD thread;
    bool descending;
};

SRWLOCK s_viewLock = SRWLOCK_INIT;
std::vector<SortView> s_views;

const Patched* Find(IShellFolder* self) {
    void** vt = *(void***)self;
    for (LONG i = 0; i < s_patchedCount; i++)
        if (s_patched[i].vtable == vt) return &s_patched[i];
    return nullptr;
}

bool IsChild(PCUIDLIST_RELATIVE pidl) {
    return pidl && pidl->mkid.cb && ((PCUIDLIST_RELATIVE)((BYTE*)pidl + pidl->mkid.cb))->mkid.cb == 0;
}

bool ReadDescending(IFolderView2* view, bool& descending) {
    SORTCOLUMN col;
    if (FAILED(view->GetSortColumns(&col, 1))) return false;
    descending = col.direction == SORT_DESCENDING;
    return true;
}

bool SortDescending(IShellFolder* folder) {
    IFolderView2* view = nullptr;
    bool descending = false;
    AcquireSRWLockShared(&s_viewLock);
    for (auto& v : s_views) {
        if (v.folder != folder) continue;
        descending = v.descending;
        if (v.thread == GetCurrentThreadId()) {
            view = v.view;
            view->AddRef();
        }
        break;
    }
    ReleaseSRWLockShared(&s_viewLock);
    if (!view) return descending;

    if (ReadDescending(view, descending)) {
        AcquireSRWLockExclusive(&s_viewLock);
        for (auto& v : s_views)
            if (v.folder == folder) v.descending = descending;
        ReleaseSRWLockExclusive(&s_viewLock);
    }
    view->Release();
    return descending;
}

bool IsSystem(IShellFolder* folder, PCUIDLIST_RELATIVE pidl) {
    DWORD attrs;
    return ItemAttributes(folder, (PCUITEMID_CHILD)pidl, attrs) && IsSystemItem(attrs);
}

HRESULT Result(int order) {
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(short)order);
}

bool IsHostColumn(IShellFolder* self, UINT column) {
    SHCOLUMNID scid;
    return column && SUCCEEDED(static_cast<IShellFolder2*>(self)->MapColumnToSCID(column, &scid)) &&
           IsEqualPropertyKey(scid, Order::HostKey());
}

std::wstring PathOf(IShellFolder* folder) {
    std::wstring path;
    IPersistFolder2* pf = nullptr;
    if (FAILED(folder->QueryInterface(IID_PPV_ARGS(&pf)))) return path;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(pf->GetCurFolder(&pidl))) {
        wchar_t buf[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, buf)) path = buf;
        CoTaskMemFree(pidl);
    }
    pf->Release();
    return path;
}

// Ranked items first in rank order, then the others by name.
HRESULT CompareByOrder(const Patched* p, IShellFolder* self, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) {
    std::wstring path = PathOf(self);
    WIN32_FIND_DATAW fa, fb;
    if (path.empty() || FAILED(SHGetDataFromIDListW(self, (PCUITEMID_CHILD)a, SHGDFIL_FINDDATA, &fa, sizeof(fa))) ||
        FAILED(SHGetDataFromIDListW(self, (PCUITEMID_CHILD)b, SHGDFIL_FINDDATA, &fb, sizeof(fb))))
        return p->compare(self, 0, a, b);
    int ra = Order::Rank(path, fa.cFileName), rb = Order::Rank(path, fb.cFileName);
    if (ra && rb && ra != rb) return Result(ra < rb ? -1 : 1);
    if (!ra != !rb) return Result(ra ? -1 : 1);
    return p->compare(self, 0, a, b);
}

HRESULT STDMETHODCALLTYPE HookCompareIDs(IShellFolder* self, LPARAM lp, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) {
    const Patched* p = Find(self);
    if (!p) return E_FAIL;
    if ((lp & ~(LPARAM)SHCIDS_COLUMNMASK) || !IsChild(a) || !IsChild(b)) return p->compare(self, lp, a, b);
    if (CurrentSettings()->systemLast) {
        bool sa = IsSystem(self, a), sb = IsSystem(self, b);
        if (sa != sb) return Result((sa ? 1 : -1) * (SortDescending(self) ? -1 : 1));
    }
    if (IsHostColumn(self, (UINT)(lp & SHCIDS_COLUMNMASK))) return CompareByOrder(p, self, a, b);
    return p->compare(self, lp, a, b);
}

bool IsHiddenName(const Settings& s, const wchar_t* name) {
    std::wstring lower = Lower(name);
    for (auto& n : s.hiddenNames)
        if (n == lower) return true;
    return false;
}

bool LooksHidden(const Settings& s, IShellFolder* folder, PCUITEMID_CHILD child) {
    WIN32_FIND_DATAW fd;
    if (FAILED(SHGetDataFromIDListW(folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)))) return false;
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || IsHiddenName(s, fd.cFileName);
}

HRESULT STDMETHODCALLTYPE HookGetAttributesOf(IShellFolder* self, UINT cidl, PCUITEMID_CHILD_ARRAY items, SFGAOF* inOut) {
    const Patched* p = Find(self);
    if (!p) return E_FAIL;
    SFGAOF wanted = inOut ? (*inOut & kHiddenFlags) : 0;
    HRESULT hr = p->attributes(self, cidl, items, inOut);
    if (FAILED(hr) || !wanted || !cidl || !items || (*inOut & wanted) == wanted || t_inAttributes) return hr;
    auto settings = CurrentSettings();
    if (!settings->hideNames || settings->hiddenNames.empty()) return hr;
    t_inAttributes = true;
    bool all = true;
    for (UINT i = 0; i < cidl && all; i++) all = LooksHidden(*settings, self, items[i]);
    t_inAttributes = false;
    if (all) *inOut |= wanted;
    return hr;
}

const IconPatched* FindIcon(IShellIcon* self) {
    void** vt = *(void***)self;
    for (LONG i = 0; i < s_iconCount; i++)
        if (s_icons[i].vtable == vt) return &s_icons[i];
    return nullptr;
}

int CustomIcon(IShellFolder* folder, PCUITEMID_CHILD child) {
    auto settings = CurrentSettings();
    if (!settings->repoIcon && settings->folderIcons.empty()) return -1;
    WIN32_FIND_DATAW fd;
    if (FAILED(SHGetDataFromIDListW(folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd))) ||
        !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return FolderIconIndex(*settings, PathOf(folder), fd);
}

HRESULT STDMETHODCALLTYPE HookGetIconOf(IShellIcon* self, PCUITEMID_CHILD child, UINT flags, int* index) {
    const IconPatched* p = FindIcon(self);
    if (!p) return E_FAIL;
    IShellFolder* folder = nullptr;
    if (index && IsChild(child) && SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&folder)))) {
        int custom = CustomIcon(folder, child);
        folder->Release();
        if (custom >= 0) {
            *index = custom;
            return S_OK;
        }
    }
    return p->iconOf(self, child, flags, index);
}

typedef HRESULT (STDMETHODCALLTYPE* GetThumbnail_t)(IThumbnailCache*, IShellItem*, UINT, WTS_FLAGS, ISharedBitmap**,
                                                    WTS_CACHEFLAGS*, WTS_THUMBNAILID*);
GetThumbnail_t s_getThumbnail;

bool HasCustomIcon(IShellItem* item) {
    PWSTR path = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    bool custom = FolderIconIndex(*CurrentSettings(), path) >= 0;
    CoTaskMemFree(path);
    return custom;
}

// A folder with a custom icon gets no content preview, as with a desktop.ini icon: the view falls back to the icon.
HRESULT STDMETHODCALLTYPE HookGetThumbnail(IThumbnailCache* self, IShellItem* item, UINT size, WTS_FLAGS flags,
                                           ISharedBitmap** bitmap, WTS_CACHEFLAGS* cacheFlags, WTS_THUMBNAILID* id) {
    if (HasCustomIcon(item)) {
        if (bitmap) *bitmap = nullptr;
        return WTS_E_FAILEDEXTRACTION;
    }
    return s_getThumbnail(self, item, size, flags, bitmap, cacheFlags, id);
}

bool PatchSlot(void** slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return true;
}

std::wstring ModuleOf(void* address) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &mod))
        GetModuleFileNameW(mod, path, ARRAYSIZE(path));
    const wchar_t* slash = wcsrchr(path, L'\\');
    return Lower(slash ? slash + 1 : path);
}

void PatchIcon(IShellFolder* folder, const wchar_t* path) {
    IShellIcon* icon = nullptr;
    if (FAILED(folder->QueryInterface(IID_PPV_ARGS(&icon)))) {
        Log(L"folders: %s has no IShellIcon", path);
        return;
    }
    void** vt = *(void***)icon;
    std::wstring module = ModuleOf(vt[kGetIconOf]);
    if (FindIcon(icon)) {
        Log(L"folders: %s shares an already patched IShellIcon vtable", path);
    } else if (module != L"windows.storage.dll" || s_iconCount >= (LONG)ARRAYSIZE(s_icons)) {
        Log(L"folders: %s IShellIcon vtable %p in %s left alone", path, vt, module.c_str());
    } else {
        IconPatched& p = s_icons[s_iconCount];
        p.vtable = vt;
        p.iconOf = (GetIconOf_t)vt[kGetIconOf];
        MemoryBarrier();
        InterlockedIncrement(&s_iconCount);
        bool ok = PatchSlot(&vt[kGetIconOf], (void*)HookGetIconOf);
        Log(L"folders: %s IShellIcon vtable %p patched (%s)", path, vt, ok ? L"ok" : L"failed");
    }
    icon->Release();
}

void PatchFolder(const wchar_t* path) {
    IShellFolder* folder = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr))) return;
    HRESULT hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
    CoTaskMemFree(pidl);
    if (FAILED(hr)) return;

    void** vt = *(void***)folder;
    std::wstring module = ModuleOf(vt[kCompareIDs]);
    if (Find(folder)) {
        Log(L"folders: %s shares an already patched vtable", path);
    } else if (module != L"windows.storage.dll" || s_patchedCount >= (LONG)ARRAYSIZE(s_patched)) {
        Log(L"folders: %s vtable %p in %s left alone", path, vt, module.c_str());
    } else {
        Patched& p = s_patched[s_patchedCount];
        p.vtable = vt;
        p.compare = (CompareIDs_t)vt[kCompareIDs];
        p.attributes = (GetAttributesOf_t)vt[kGetAttributesOf];
        MemoryBarrier();
        InterlockedIncrement(&s_patchedCount);
        bool ok = PatchSlot(&vt[kCompareIDs], (void*)HookCompareIDs) &&
                  PatchSlot(&vt[kGetAttributesOf], (void*)HookGetAttributesOf);
        Log(L"folders: %s vtable %p in %s patched (%s)", path, vt, module.c_str(), ok ? L"ok" : L"failed");
    }
    PatchIcon(folder, path);
    folder->Release();
}

void PatchThumbnails() {
    IThumbnailCache* cache = nullptr;
    if (FAILED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache)))) {
        Log(L"folders: thumbnail cache not created");
        return;
    }
    void** vt = *(void***)cache;
    std::wstring module = ModuleOf(vt[3]);
    if (module != L"thumbcache.dll") {
        Log(L"folders: IThumbnailCache vtable %p in %s left alone", vt, module.c_str());
    } else {
        s_getThumbnail = (GetThumbnail_t)vt[3];
        bool ok = PatchSlot(&vt[3], (void*)HookGetThumbnail);
        Log(L"folders: IThumbnailCache vtable %p patched (%s)", vt, ok ? L"ok" : L"failed");
    }
    cache->Release();
}

BOOL CALLBACK Install(PINIT_ONCE, void*, void**) {
    wchar_t windows[MAX_PATH];
    UINT n = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (!n || n >= ARRAYSIZE(windows)) return TRUE;
    wchar_t root[4] = { windows[0], L':', L'\\', 0 };
    PatchFolder(windows);
    PatchFolder(root);
    PatchThumbnails();
    return TRUE;
}

}

void InstallFolderHooks() {
    InitOnceExecuteOnce(&s_once, Install, nullptr, nullptr);
}

bool ItemAttributes(IShellFolder* folder, PCUITEMID_CHILD child, DWORD& attrs) {
    WIN32_FIND_DATAW fd;
    if (FAILED(SHGetDataFromIDListW(folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)))) return false;
    attrs = fd.dwFileAttributes;
    return true;
}

bool IsSystemItem(DWORD attrs) {
    const DWORD both = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    return (attrs & both) == both;
}

void RegisterSortView(IShellFolder* folder, IFolderView* view) {
    IFolderView2* view2 = nullptr;
    if (!folder || !view || FAILED(view->QueryInterface(IID_PPV_ARGS(&view2)))) return;
    bool descending = false;
    ReadDescending(view2, descending);
    AcquireSRWLockExclusive(&s_viewLock);
    s_views.push_back({ folder, view2, GetCurrentThreadId(), descending });
    ReleaseSRWLockExclusive(&s_viewLock);
}

void UnregisterSortView(IShellFolder* folder) {
    IFolderView2* view = nullptr;
    AcquireSRWLockExclusive(&s_viewLock);
    for (auto it = s_views.begin(); it != s_views.end(); ++it) {
        if (it->folder != folder) continue;
        view = it->view;
        s_views.erase(it);
        break;
    }
    ReleaseSRWLockExclusive(&s_viewLock);
    if (view) view->Release();
}
