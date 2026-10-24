#include "RepoInfo.h"
#include "GitStatus.h"
#include "LinguistData.h"
#include <algorithm>
#include <deque>
#include <map>
#include <regex>
#include <shlobj.h>

namespace {

const ULONGLONG kRecheckMs = 5000;
const UINT64 kMaxIconBytes = 2 * 1024 * 1024;
const UINT32 kOtherColor = 0x8B949E;
const size_t kMaxLanguages = 6;
const unsigned short kUncounted = 65535;
const char kCacheHeader[] = "ExplorerTint repos 3\n";  // bumped when the statistics change

struct Entry {
    std::wstring commit;
    RepoInfo info;
    bool ready = false;
    ULONGLONG checked = 0;
};

SRWLOCK s_lock = SRWLOCK_INIT;
std::map<std::wstring, Entry> s_entries;  // key: lowercase repository path
std::deque<std::wstring> s_queue;
std::wstring s_busy;  // the repository the worker is computing
HANDLE s_wake = nullptr;
INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string ReadText(const std::wstring& path) {
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    char buf[65536];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    CloseHandle(f);
    return text;
}

// Git's test: a NUL byte in the first 8000 bytes.
bool IsBinary(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[8000];
    DWORD got = 0;
    bool binary = ReadFile(f, buf, sizeof(buf), &got, nullptr) && memchr(buf, 0, got) != nullptr;
    CloseHandle(f);
    return binary;
}

std::string FirstLine(const std::string& s) {
    size_t end = s.find_first_of("\r\n");
    return s.substr(0, end);
}

std::wstring Resolve(const std::wstring& base, const std::string& rel) {
    std::wstring r = Widen(rel);
    std::replace(r.begin(), r.end(), L'/', L'\\');
    if (r.size() > 1 && (r[1] == L':' || (r[0] == L'\\' && r[1] == L'\\'))) return r;
    return base + L"\\" + r;
}

// HEAD commit read from the files of the repository, without running git; empty for an unborn branch.
std::wstring HeadCommit(const std::wstring& repo) {
    std::wstring root = repo.back() == L'\\' ? repo.substr(0, repo.size() - 1) : repo;
    std::wstring gitDir = root + L"\\.git";
    DWORD attrs = GetFileAttributesW(gitDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return L"";
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string line = FirstLine(ReadText(gitDir));
        if (line.compare(0, 8, "gitdir: ") != 0) return L"";
        gitDir = Resolve(root, line.substr(8));
    }
    std::string common = FirstLine(ReadText(gitDir + L"\\commondir"));
    std::wstring commonDir = common.empty() ? gitDir : Resolve(gitDir, common);
    std::string head = FirstLine(ReadText(gitDir + L"\\HEAD"));
    if (head.compare(0, 5, "ref: ") != 0) return Widen(head);
    std::string ref = head.substr(5);
    std::wstring refPath = Widen(ref);
    std::replace(refPath.begin(), refPath.end(), L'/', L'\\');
    for (auto& dir : { gitDir, commonDir }) {
        std::string sha = FirstLine(ReadText(dir + L"\\" + refPath));
        if (!sha.empty()) return Widen(sha);
    }
    std::string packed = ReadText(commonDir + L"\\packed-refs");
    for (size_t pos = 0; pos < packed.size();) {
        size_t end = packed.find('\n', pos);
        std::string line = packed.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t space = line.find(' ');
        if (space != std::string::npos && line.compare(space + 1, std::string::npos, ref) == 0) return Widen(line.substr(0, space));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return L"";
}

template <size_t N> int FindKey(const LinguistKey (&table)[N], const wchar_t* key) {
    auto it = std::lower_bound(std::begin(table), std::end(table), key,
                               [](const LinguistKey& k, const wchar_t* v) { return wcscmp(k.key, v) < 0; });
    return it != std::end(table) && !wcscmp(it->key, key) ? (int)(it - std::begin(table)) : -1;
}

template <size_t N> void Candidates(const LinguistKey (&table)[N], int first, std::vector<int>& out) {
    for (size_t i = first; i < N && !wcscmp(table[i].key, table[first].key); i++) out.push_back(table[i].language);
}

// Filename first, then the longest matching extension, as linguist does.
std::vector<int> LanguagesOf(const std::wstring& name) {
    std::vector<int> langs;
    int i = FindKey(kLinguistFilenames, name.c_str());
    if (i >= 0) {
        Candidates(kLinguistFilenames, i, langs);
        return langs;
    }
    std::wstring lower = Lower(name);
    for (size_t dot = lower.find(L'.'); dot != std::wstring::npos; dot = lower.find(L'.', dot + 1)) {
        i = FindKey(kLinguistExtensions, lower.c_str() + dot);
        if (i >= 0) {
            Candidates(kLinguistExtensions, i, langs);
            break;
        }
    }
    return langs;
}

struct Patterns {
    std::vector<std::wregex> list;
    template <size_t N> void Add(const wchar_t* const (&sources)[N]) {
        for (auto s : sources) {
            try {
                list.emplace_back(s, std::regex::ECMAScript | std::regex::optimize);
            } catch (const std::regex_error&) {
                Log(L"languages: pattern skipped: %s", s);
            }
        }
    }
    bool Match(const std::wstring& path) const {
        for (auto& r : list)
            if (std::regex_search(path, r)) return true;
        return false;
    }
};

const Patterns& Excluded() {
    static const Patterns patterns = [] {
        Patterns p;
        p.Add(kLinguistVendored);
        p.Add(kLinguistDocumentation);
        return p;
    }();
    return patterns;
}

int LanguageIndex(const wchar_t* name) {
    for (size_t i = 0; i < ARRAYSIZE(kLinguistLanguages); i++)
        if (!wcscmp(kLinguistLanguages[i].name, name)) return (int)i;
    return kUncounted;
}

// linguist's heuristics.yml rule for ".h": Objective-C, then C++, else C. Patterns are matched line by line.
int HeaderLanguage(const std::wstring& file) {
    static const int objc = LanguageIndex(L"Objective-C"), cpp = LanguageIndex(L"C++"), c = LanguageIndex(L"C");
    static const std::regex objcPattern(
        R"(^\s*(@(interface|class|protocol|property|end|synchronised|selector|implementation)\b|#import\s+.+\.h[">]))");
    static const std::regex cppPatterns[] = {
        std::regex(R"(^\s*#\s*include <(cstdint|string|vector|map|list|array|bitset|queue|stack|forward_list|unordered_map|unordered_set|(i|o|io)stream)>)"),
        std::regex(R"(^\s*template\s*<)"),
        std::regex(R"(^[ \t]*(try|constexpr))"),
        std::regex(R"(^[ \t]*catch\s*\()"),
        std::regex(R"(^[ \t]*(class|(using[ \t]+)?namespace)\s+\w+)"),
        std::regex(R"(^[ \t]*(private|public|protected):$)"),
        std::regex(R"(__has_cpp_attribute|__cplusplus >)"),
        std::regex(R"(std::\w+)"),
    };
    static const char* const cppWords[] = { "include", "template", "try", "constexpr", "catch", "class", "namespace",
                                            "private", "public", "protected", "__", "std::" };
    std::string text = ReadText(file);
    bool isCpp = false;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if ((line.find('@') != std::string::npos || line.find("#import") != std::string::npos) && std::regex_search(line, objcPattern))
            return objc;
        if (isCpp) continue;
        for (auto word : cppWords) {
            if (line.find(word) == std::string::npos) continue;
            for (auto& r : cppPatterns)
                if (std::regex_search(line, r)) isCpp = true;
            break;
        }
    }
    return isCpp ? cpp : c;
}

// A PNG must be roughly square and at least 16 pixels to stand for the project (a banner logo does not).
bool IconShaped(const std::wstring& file) {
    if (file.size() < 4 || _wcsicmp(file.c_str() + file.size() - 4, L".png") != 0) return true;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BYTE h[24];
    DWORD got = 0;
    bool read = ReadFile(f, h, sizeof(h), &got, nullptr) && got == sizeof(h);
    CloseHandle(f);
    if (!read || memcmp(h, "\x89PNG", 4) != 0 || memcmp(h + 12, "IHDR", 4) != 0) return false;
    UINT32 w = _byteswap_ulong(*(UINT32*)(h + 16)), hgt = _byteswap_ulong(*(UINT32*)(h + 20));
    UINT32 lo = std::min(w, hgt), hi = std::max(w, hgt);
    return lo >= 16 && hi * 4 <= lo * 5;
}

int IconScore(const std::wstring& path, UINT64 size) {
    std::wstring lower = Lower(path);
    size_t slash = lower.find_last_of(L'/');
    std::wstring name = lower.substr(slash == std::wstring::npos ? 0 : slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || size == 0 || size > kMaxIconBytes) return 0;
    std::wstring stem = name.substr(0, dot), ext = name.substr(dot);
    if (ext != L".ico" && ext != L".png") return 0;
    int score = ext == L".ico" ? 3 : 1;
    if (stem == L"icon" || stem == L"app" || stem == L"appicon" || stem == L"favicon" || stem == L"logo" ||
        stem == L"ic_launcher")
        score += 10;
    else if (stem.find(L"icon") != std::wstring::npos || stem.find(L"logo") != std::wstring::npos)
        score += 5;
    else if (ext != L".ico")
        return 0;
    if (lower.find(L"xxxhdpi") != std::wstring::npos) score += 3;
    else if (lower.find(L"xxhdpi") != std::wstring::npos) score += 2;
    else if (lower.find(L"xhdpi") != std::wstring::npos) score += 1;
    if (lower.find(L"test") != std::wstring::npos || lower.find(L"example") != std::wstring::npos) score -= 5;
    score -= (int)std::count(lower.begin(), lower.end(), L'/');
    return score;
}

RepoInfo Compute(const std::wstring& repo) {
    RepoInfo info;
    std::string out;
    if (!GitStatus::Run(repo, L"ls-tree -r -z -l HEAD", out)) return info;

    struct Pending {
        std::vector<int> langs;
        UINT64 size;
    };
    std::vector<UINT64> bytes(ARRAYSIZE(kLinguistLanguages));
    std::vector<Pending> ambiguous;
    const Patterns& excluded = Excluded();
    int bestIcon = 4;
    std::wstring icon;

    // "<mode> <type> <object> <size>\t<path>\0"
    for (size_t pos = 0; pos < out.size();) {
        size_t end = out.find('\0', pos);
        if (end == std::string::npos) end = out.size();
        std::string rec = out.substr(pos, end - pos);
        pos = end + 1;
        size_t tab = rec.find('\t');
        if (tab == std::string::npos || rec.compare(0, 6, "120000") == 0 || rec.find(" blob ") == std::string::npos) continue;
        size_t sizeStart = rec.find_last_of(' ', tab);
        UINT64 size = _strtoui64(rec.c_str() + sizeStart + 1, nullptr, 10);
        std::wstring path = Widen(rec.substr(tab + 1));
        size_t slash = path.find_last_of(L'/');
        std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);

        std::wstring file = repo + (repo.back() == L'\\' ? L"" : L"\\") + path;
        std::replace(file.begin(), file.end(), L'/', L'\\');
        int score = IconScore(path, size);
        if (score > bestIcon) {
            if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && IconShaped(file)) {
                bestIcon = score;
                icon = file;
            }
        }

        std::vector<int> langs = LanguagesOf(name);
        if (langs.empty() || (langs.size() == 1 && langs[0] == kUncounted) || excluded.Match(path) || IsBinary(file))
            continue;
        if (langs.size() > 1 && Lower(name.substr(name.size() - std::min<size_t>(name.size(), 2))) == L".h") {
            int lang = HeaderLanguage(file);
            if (lang != kUncounted) bytes[lang] += size;
        } else if (langs.size() == 1) bytes[langs[0]] += size;
        else ambiguous.push_back({ std::move(langs), size });
    }
    // An ambiguous extension goes to the candidate the rest of the repository uses most, else to the likeliest.
    std::vector<UINT64> known = bytes;
    auto knownOf = [&](int l) { return l == kUncounted ? 0 : known[l]; };
    for (auto& a : ambiguous) {
        int best = a.langs[0];
        for (int l : a.langs)
            if (knownOf(l) > knownOf(best)) best = l;
        if (best != kUncounted) bytes[best] += a.size;
    }

    UINT64 total = 0;
    for (auto b : bytes) total += b;
    std::vector<int> order;
    for (size_t i = 0; i < bytes.size(); i++)
        if (bytes[i]) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bytes[a] > bytes[b]; });
    double other = 0;
    for (size_t i = 0; i < order.size(); i++) {
        double share = (double)bytes[order[i]] / total;
        if (i < kMaxLanguages - 1 || order.size() == kMaxLanguages) {
            info.languages.push_back({ kLinguistLanguages[order[i]].name, kLinguistLanguages[order[i]].color, share });
        } else {
            other += share;
        }
    }
    if (other > 0) info.languages.push_back({ L"Other", kOtherColor, other });
    info.icon = icon;
    return info;
}

std::wstring CachePath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = std::wstring(base) + L"\\ExplorerTint\\repos.txt";
    CoTaskMemFree(base);
    return path;
}

// A header, then one line per repository: path \t commit \t icon \t name|RRGGBB|share;...
void SaveCache() {
    std::string text = kCacheHeader;
    AcquireSRWLockShared(&s_lock);
    for (auto& [key, e] : s_entries) {
        if (!e.ready) continue;
        text += Narrow(key) + "\t" + Narrow(e.commit) + "\t" + Narrow(e.info.icon) + "\t";
        for (auto& l : e.info.languages) {
            char buf[64];
            sprintf_s(buf, "|%06X|%.4f;", l.color, l.share);
            text += Narrow(l.name) + buf;
        }
        text += "\n";
    }
    ReleaseSRWLockShared(&s_lock);
    std::wstring path = CachePath(), temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

void LoadCache() {
    std::string text = ReadText(CachePath());
    if (text.compare(0, sizeof(kCacheHeader) - 1, kCacheHeader) != 0) return;
    for (size_t pos = sizeof(kCacheHeader) - 1; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1), t3 = line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) continue;
        Entry e;
        e.commit = Widen(line.substr(t1 + 1, t2 - t1 - 1));
        e.info.icon = Widen(line.substr(t2 + 1, t3 - t2 - 1));
        e.ready = true;
        std::string langs = line.substr(t3 + 1);
        for (size_t p = 0; p < langs.size();) {
            size_t semi = langs.find(';', p);
            if (semi == std::string::npos) break;
            std::string item = langs.substr(p, semi - p);
            p = semi + 1;
            size_t b1 = item.find('|'), b2 = item.find('|', b1 + 1);
            if (b1 == std::string::npos || b2 == std::string::npos) continue;
            e.info.languages.push_back({ Widen(item.substr(0, b1)), (UINT32)strtoul(item.c_str() + b1 + 1, nullptr, 16),
                                         atof(item.c_str() + b2 + 1) });
        }
        s_entries[Widen(line.substr(0, t1))] = std::move(e);
    }
}

DWORD WINAPI Worker(void*) {
    for (;;) {
        WaitForSingleObject(s_wake, INFINITE);
        for (;;) {
            AcquireSRWLockExclusive(&s_lock);
            if (s_queue.empty()) {
                ReleaseSRWLockExclusive(&s_lock);
                break;
            }
            std::wstring repo = s_queue.front();
            s_queue.pop_front();
            s_busy = repo;
            ReleaseSRWLockExclusive(&s_lock);

            ULONGLONG start = GetTickCount64();
            std::wstring commit = HeadCommit(repo);
            RepoInfo info = commit.empty() ? RepoInfo() : Compute(repo);
            Log(L"languages: %s, %zu language(s), icon \"%s\", %llu ms", repo.c_str(), info.languages.size(),
                info.icon.c_str(), GetTickCount64() - start);
            AcquireSRWLockExclusive(&s_lock);
            Entry& e = s_entries[Lower(repo)];
            e.commit = commit;
            e.info = std::move(info);
            e.ready = true;
            e.checked = GetTickCount64();
            s_busy.clear();
            ReleaseSRWLockExclusive(&s_lock);
            SaveCache();
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, repo.c_str(), nullptr);
        }
    }
}

BOOL CALLBACK Init(PINIT_ONCE, void*, void**) {
    LoadCache();
    s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = s_wake ? CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr) : nullptr;
    if (t) CloseHandle(t);
    return TRUE;
}

}

bool GetRepoInfo(const std::wstring& repo, RepoInfo& out) {
    InitOnceExecuteOnce(&s_once, Init, nullptr, nullptr);
    std::wstring key = Lower(repo);
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&s_lock);
    auto it = s_entries.find(key);
    bool known = it != s_entries.end() && it->second.ready;
    bool fresh = known && now - it->second.checked < kRecheckMs;
    std::wstring commit = known ? it->second.commit : L"";
    if (known) out = it->second.info;
    ReleaseSRWLockShared(&s_lock);
    if (fresh) return true;

    bool current = known && HeadCommit(repo) == commit;
    AcquireSRWLockExclusive(&s_lock);
    if (current) {
        s_entries[key].checked = now;
    } else if (_wcsicmp(s_busy.c_str(), repo.c_str()) != 0 && std::find(s_queue.begin(), s_queue.end(), repo) == s_queue.end()) {
        s_queue.push_back(repo);
        SetEvent(s_wake);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return known;
}
