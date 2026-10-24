#include "Order.h"
#include <map>
#include <unordered_map>
#include <shlobj.h>

namespace {

struct FolderOrder {
    std::wstring path;                              // as written in the file
    std::vector<std::wstring> names;                // as written in the file
    std::unordered_map<std::wstring, int> ranks;    // lowercase name -> 1-based rank
};

SRWLOCK s_lock = SRWLOCK_INIT;
std::map<std::wstring, FolderOrder> s_orders;       // key: normalized folder path
FILETIME s_stamp;
ULONGLONG s_checked;

INIT_ONCE s_hostOnce = INIT_ONCE_STATIC_INIT;
PROPERTYKEY s_hostKey;
std::wstring s_hostLabel;

std::wstring FilePath() {
    wchar_t* base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = base;
    CoTaskMemFree(base);
    return path + L"\\ExplorerTint\\order.ini";
}

std::wstring Key(std::wstring path) {
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return Lower(path);
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r"), b = s.find_last_not_of(L" \t\r");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

void AddName(FolderOrder& o, const std::wstring& name) {
    std::wstring lower = Lower(name);
    if (o.ranks.count(lower)) return;
    o.names.push_back(name);
    o.ranks[lower] = (int)o.names.size();
}

bool Stamp(FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(FilePath().c_str(), GetFileExInfoStandard, &data)) return false;
    ft = data.ftLastWriteTime;
    return true;
}

void Parse(const std::wstring& text) {
    s_orders.clear();
    FolderOrder* current = nullptr;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == L';') continue;
        if (line.front() == L'[' && line.back() == L']') {
            std::wstring path = line.substr(1, line.size() - 2);
            current = &s_orders[Key(path)];
            current->path = path;
        } else if (current) {
            AddName(*current, line);
        }
    }
}

// Caller holds the exclusive lock.
void ReloadIfChanged() {
    ULONGLONG now = GetTickCount64();
    if (s_checked && now - s_checked < 500) return;
    s_checked = now;
    FILETIME ft = {};
    if (!Stamp(ft)) {
        s_orders.clear();
        s_stamp = ft;
        return;
    }
    if (CompareFileTime(&ft, &s_stamp) == 0) return;
    s_stamp = ft;

    HANDLE f = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string bytes;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 24)) {
        bytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!bytes.empty() && !ReadFile(f, &bytes[0], (DWORD)bytes.size(), &read, nullptr)) read = 0;
        bytes.resize(read);
    }
    CloseHandle(f);
    size_t skip = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::wstring text;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), nullptr, 0);
    if (n > 0) {
        text.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), &text[0], n);
    }
    Parse(text);
    Log(L"order: %u folder(s) loaded", (unsigned)s_orders.size());
}

// Caller holds the exclusive lock.
void Save() {
    std::wstring text;
    for (auto& entry : s_orders) {
        if (entry.second.names.empty()) continue;
        text += L"[" + entry.second.path + L"]\r\n";
        for (auto& name : entry.second.names) text += name + L"\r\n";
        text += L"\r\n";
    }
    std::string bytes;
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        bytes.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &bytes[0], n, nullptr, nullptr);
    }

    std::wstring path = FilePath();
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"order: cannot write %s (%lu)", temp.c_str(), GetLastError());
        return;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"order: cannot replace %s (%lu)", path.c_str(), GetLastError());
        return;
    }
    Stamp(s_stamp);
}

BOOL CALLBACK InitHost(PINIT_ONCE, void*, void**) {
    if (FAILED(PSGetPropertyKeyFromName(L"System.ItemFolderNameDisplay", &s_hostKey))) return TRUE;
    IPropertyDescription* pd = nullptr;
    if (SUCCEEDED(PSGetPropertyDescription(s_hostKey, IID_PPV_ARGS(&pd)))) {
        PWSTR label = nullptr;
        if (SUCCEEDED(pd->GetDisplayName(&label))) s_hostLabel = label;
        CoTaskMemFree(label);
        pd->Release();
    }
    Log(L"order: host column \"%s\"", s_hostLabel.c_str());
    return TRUE;
}

}

namespace Order {

int Rank(const std::wstring& folder, const std::wstring& name) {
    AcquireSRWLockExclusive(&s_lock);
    ReloadIfChanged();
    int rank = 0;
    auto it = s_orders.find(Key(folder));
    if (it != s_orders.end()) {
        auto r = it->second.ranks.find(Lower(name));
        if (r != it->second.ranks.end()) rank = r->second;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return rank;
}

bool Has(const std::wstring& folder) {
    AcquireSRWLockExclusive(&s_lock);
    ReloadIfChanged();
    auto it = s_orders.find(Key(folder));
    bool has = it != s_orders.end() && !it->second.names.empty();
    ReleaseSRWLockExclusive(&s_lock);
    return has;
}

void Set(const std::wstring& folder, const std::vector<std::wstring>& names) {
    AcquireSRWLockExclusive(&s_lock);
    s_checked = 0;
    ReloadIfChanged();
    FolderOrder& o = s_orders[Key(folder)];
    o = FolderOrder();
    o.path = folder;
    for (auto& name : names) AddName(o, name);
    Save();
    ReleaseSRWLockExclusive(&s_lock);
}

const PROPERTYKEY& HostKey() {
    InitOnceExecuteOnce(&s_hostOnce, InitHost, nullptr, nullptr);
    return s_hostKey;
}

const std::wstring& HostLabel() {
    InitOnceExecuteOnce(&s_hostOnce, InitHost, nullptr, nullptr);
    return s_hostLabel;
}

}
