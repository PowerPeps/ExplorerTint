#include "GitStatus.h"
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

struct Entry {
    GitState state;
    bool dir;
};

struct Repo {
    std::wstring root;
    std::unordered_map<std::wstring, Entry> entries;
    ULONGLONG due = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::wstring, std::unique_ptr<Repo>> g_repos;
std::unordered_map<std::wstring, std::wstring> g_rootOf;
std::vector<HWND> g_listeners;
std::wstring g_gitPath;
HANDLE g_wake = nullptr;
UINT g_msgRefresh = 0;
INIT_ONCE g_init = INIT_ONCE_STATIC_INIT;

const DWORD kDebounceMs = 300;
const DWORD kGitTimeoutMs = 30000;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring FindRoot(std::wstring p) {
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    for (;;) {
        size_t pos = p.find_last_of(L'\\');
        std::wstring leaf = p.substr(pos == std::wstring::npos ? 0 : pos + 1);
        if (_wcsicmp(leaf.c_str(), L".git") == 0) return L"";
        if (GetFileAttributesW((p + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES)
            return (p.size() == 2 && p[1] == L':') ? p + L"\\" : p;
        if (pos == std::wstring::npos) return L"";
        p.resize(pos);
    }
}

bool RunGit(const std::wstring& root, std::string& out) {
    std::wstring dir = root;
    if (dir.back() == L'\\') dir += L'.';
    AcquireSRWLockShared(&g_lock);
    std::wstring exe = g_gitPath.empty() ? L"git.exe" : g_gitPath;
    ReleaseSRWLockShared(&g_lock);
    std::wstring cmd = L"\"" + exe + L"\" --no-optional-locks -C \"" + dir +
                       L"\" status --porcelain=v1 -z --ignored=traditional --untracked-files=normal";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    HANDLE inherit[2] = { wr, nul };
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                        sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    ok = ok && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                              &si.StartupInfo, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        Log(L"git: cannot start \"%s\" (error %lu)", exe.c_str(), GetLastError());
        CloseHandle(rd);
        return false;
    }

    char buf[65536];
    DWORD got;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(rd);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, kGitTimeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) Log(L"git: status failed in %s (exit %lu)", root.c_str(), code);
    return code == 0;
}

// Porcelain v1 -z records: "XY path\0", renames and copies add "origPath\0".
void Parse(const std::string& out, std::unordered_map<std::wstring, Entry>& map) {
    size_t i = 0;
    while (i + 3 < out.size()) {
        size_t end = out.find('\0', i);
        if (end == std::string::npos) break;
        char x = out[i], y = out[i + 1];
        std::string path = out.substr(i + 3, end - i - 3);
        i = end + 1;
        if (x == 'R' || x == 'C') {
            size_t orig = out.find('\0', i);
            i = orig == std::string::npos ? out.size() : orig + 1;
        }

        GitState s;
        if (x == '?' && y == '?') s = GitState::Untracked;
        else if (x == '!' && y == '!') s = GitState::Ignored;
        else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) continue;
        else if (x != ' ') s = GitState::Staged;
        else continue;

        bool dir = !path.empty() && path.back() == '/';
        if (dir) path.pop_back();
        std::wstring key = Lower(Utf8ToWide(path));
        std::replace(key.begin(), key.end(), L'/', L'\\');
        map[key] = { s, dir };
    }
}

void Refresh(Repo* r) {
    ULONGLONG start = GetTickCount64();
    std::string out;
    if (!RunGit(r->root, out)) return;
    std::unordered_map<std::wstring, Entry> entries;
    Parse(out, entries);

    std::vector<HWND> listeners;
    AcquireSRWLockExclusive(&g_lock);
    r->entries.swap(entries);
    size_t count = r->entries.size();
    listeners = g_listeners;
    ReleaseSRWLockExclusive(&g_lock);

    Log(L"git: %s -> %zu entries in %llu ms", r->root.c_str(), count, GetTickCount64() - start);
    for (HWND h : listeners) PostMessageW(h, g_msgRefresh, 0, 0);
}

void Schedule(Repo* r, DWORD delayMs) {
    AcquireSRWLockExclusive(&g_lock);
    r->due = GetTickCount64() + delayMs;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
}

DWORD WINAPI Worker(void*) {
    for (;;) {
        std::vector<Repo*> ready;
        DWORD wait = INFINITE;
        ULONGLONG now = GetTickCount64();
        AcquireSRWLockExclusive(&g_lock);
        for (auto& kv : g_repos) {
            Repo* r = kv.second.get();
            if (!r->due) continue;
            if (r->due <= now) {
                r->due = 0;
                ready.push_back(r);
            } else {
                wait = std::min(wait, (DWORD)(r->due - now));
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        for (Repo* r : ready) Refresh(r);
        if (ready.empty()) WaitForSingleObject(g_wake, wait);
    }
}

DWORD WINAPI Watcher(void* param) {
    Repo* r = (Repo*)param;
    HANDLE dir = CreateFileW(r->root.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        Log(L"git: cannot watch %s (error %lu)", r->root.c_str(), GetLastError());
        return 0;
    }
    alignas(DWORD) BYTE buf[16384];
    DWORD got;
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    while (ReadDirectoryChangesW(dir, buf, sizeof(buf), TRUE, filter, &got, nullptr, nullptr))
        Schedule(r, kDebounceMs);
    CloseHandle(dir);
    return 0;
}

BOOL CALLBACK Init(PINIT_ONCE, void*, void**) {
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_msgRefresh = RegisterWindowMessageW(L"ExplorerTint.Refresh");
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}

void EnsureInit() {
    InitOnceExecuteOnce(&g_init, Init, nullptr, nullptr);
}

}

namespace GitStatus {

std::wstring RepoFor(const std::wstring& folder) {
    EnsureInit();
    std::wstring key = Lower(folder);
    AcquireSRWLockShared(&g_lock);
    auto it = g_rootOf.find(key);
    bool known = it != g_rootOf.end();
    std::wstring root = known ? it->second : std::wstring();
    ReleaseSRWLockShared(&g_lock);
    if (known) return root;

    root = Lower(FindRoot(folder));
    Repo* added = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    g_rootOf[key] = root;
    if (!root.empty() && !g_repos.count(root)) {
        auto r = std::make_unique<Repo>();
        r->root = root;
        added = r.get();
        g_repos[root] = std::move(r);
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (added) {
        Log(L"git: tracking %s", root.c_str());
        HANDLE t = CreateThread(nullptr, 0, Watcher, added, 0, nullptr);
        if (t) CloseHandle(t);
        Schedule(added, 0);
    }
    return root;
}

GitState Query(const std::wstring& root, const std::wstring& relPath) {
    std::wstring key = Lower(relPath);
    GitState s = GitState::None;
    AcquireSRWLockShared(&g_lock);
    auto it = g_repos.find(root);
    if (it != g_repos.end()) {
        const auto& entries = it->second->entries;
        auto hit = entries.find(key);
        if (hit != entries.end()) {
            s = hit->second.state;
        } else {
            for (size_t pos = key.find(L'\\'); pos != std::wstring::npos; pos = key.find(L'\\', pos + 1)) {
                auto parent = entries.find(key.substr(0, pos));
                if (parent != entries.end() && parent->second.dir && parent->second.state != GitState::Staged) {
                    s = parent->second.state;
                    break;
                }
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return s;
}

void AddListener(HWND hwnd) {
    AcquireSRWLockExclusive(&g_lock);
    g_listeners.push_back(hwnd);
    ReleaseSRWLockExclusive(&g_lock);
}

void RemoveListener(HWND hwnd) {
    AcquireSRWLockExclusive(&g_lock);
    g_listeners.erase(std::remove(g_listeners.begin(), g_listeners.end(), hwnd), g_listeners.end());
    ReleaseSRWLockExclusive(&g_lock);
}

void SetGitPath(const std::wstring& path) {
    AcquireSRWLockExclusive(&g_lock);
    g_gitPath = path;
    ReleaseSRWLockExclusive(&g_lock);
}

UINT RefreshMessage() {
    EnsureInit();
    return g_msgRefresh;
}

}
