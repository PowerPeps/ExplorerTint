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
const char kCacheHeader[] = "ExplorerTint repos 5\n";  // bumped when the statistics change

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

const wchar_t kUnborn[] = L"unborn ";

ULONGLONG LastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// The HEAD commit, else, on an unborn branch, "unborn <time>" from the last change of the root folder or the index.
std::wstring StateKey(const std::wstring& repo) {
    std::wstring commit = HeadCommit(repo);
    if (!commit.empty()) return commit;
    std::wstring root = repo.back() == L'\\' ? repo.substr(0, repo.size() - 1) : repo;
    if (GetFileAttributesW((root + L"\\.git").c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    return kUnborn + std::to_wstring(std::max(LastWrite(root), LastWrite(root + L"\\.git\\index")));
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

struct TreeFile {
    std::wstring path;  // '/' separated, as in the tree
    UINT64 size;
};

std::wstring WorkFile(const std::wstring& repo, std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return repo + (repo.back() == L'\\' ? L"" : L"\\") + path;
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L'/');
    return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

std::wstring NameOf(const std::wstring& path) {
    return path.substr(DirOf(path).size());
}

std::wstring ExtOf(const std::wstring& path) {
    std::wstring name = NameOf(path);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? L"" : Lower(name.substr(dot));
}

int Depth(const std::wstring& path) {
    return (int)std::count(path.begin(), path.end(), L'/');
}

// A reference written in a file of the tree directory dir ("../assets/icon.ico", "res\\app.ico"), resolved and
// normalised; empty when it leaves the repository.
std::wstring JoinTree(const std::wstring& dir, std::wstring ref) {
    std::replace(ref.begin(), ref.end(), L'\\', L'/');
    std::wstring all = !ref.empty() && ref[0] == L'/' ? ref.substr(1) : dir + ref;
    std::vector<std::wstring> parts;
    for (size_t pos = 0; pos <= all.size();) {
        size_t end = all.find(L'/', pos);
        if (end == std::wstring::npos) end = all.size();
        std::wstring part = all.substr(pos, end - pos);
        pos = end + 1;
        if (part == L"..") {
            if (parts.empty()) return L"";
            parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
    }
    std::wstring out;
    for (auto& p : parts) out += (out.empty() ? L"" : L"/") + p;
    return out;
}

std::vector<std::string> Lines(const std::string& text, size_t max = SIZE_MAX) {
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < text.size() && lines.size() < max;) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = end + 1;
    }
    return lines;
}

std::string Unquote(std::string s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// The project's icon: first one its own files declare (Windows resource script, Android manifest, web manifest,
// installer script, plugin package, web page, top of the README), else the best named image file.
class IconFinder {
public:
    IconFinder(const std::wstring& repo, const std::vector<TreeFile>& files) : m_repo(repo), m_files(files) {
        for (auto& f : files) m_index[Lower(f.path)] = &f;
        std::wstring root = repo.back() == L'\\' ? repo.substr(0, repo.size() - 1) : repo;
        m_name = Lower(root.substr(root.find_last_of(L'\\') + 1));
    }

    std::wstring Find() {
        typedef std::wstring (IconFinder::*Reader)(const TreeFile&);
        struct Source {
            bool (*match)(const std::wstring& name, const std::wstring& ext);
            Reader read;
        };
        static const Source sources[] = {
            { [](const std::wstring&, const std::wstring& ext) { return ext == L".rc"; }, &IconFinder::FromRc },
            { [](const std::wstring& name, const std::wstring&) { return name == L"androidmanifest.xml"; }, &IconFinder::FromAndroid },
            { [](const std::wstring& name, const std::wstring&) { return name == L"manifest.json" || name == L"site.webmanifest" || name == L"manifest.webmanifest"; }, &IconFinder::FromWebManifest },
            { [](const std::wstring&, const std::wstring& ext) { return ext == L".iss" || ext == L".nsi"; }, &IconFinder::FromInstaller },
            { [](const std::wstring& name, const std::wstring&) { return name == L"loupedeckpackage.yaml"; }, &IconFinder::FromLoupedeck },
            { [](const std::wstring& name, const std::wstring&) { return name == L"index.html"; }, &IconFinder::FromHtml },
            { [](const std::wstring& name, const std::wstring&) { return name == L"readme.md"; }, &IconFinder::FromReadme },
        };
        for (auto& source : sources) {
            std::vector<const TreeFile*> found;
            for (auto& f : m_files) {
                std::wstring lower = Lower(f.path);
                if (source.match(NameOf(lower), ExtOf(lower)) && lower.find(L"test") == std::wstring::npos) found.push_back(&f);
            }
            std::sort(found.begin(), found.end(), [](const TreeFile* a, const TreeFile* b) {
                int da = Depth(a->path), db = Depth(b->path);
                return da != db ? da < db : a->path < b->path;
            });
            for (auto f : found) {
                std::wstring icon = (this->*source.read)(*f);
                if (!icon.empty()) return icon;
            }
        }
        return ByName();
    }

private:
    const std::wstring& m_repo;
    const std::vector<TreeFile>& m_files;
    std::map<std::wstring, const TreeFile*> m_index;  // key: lowercase path
    std::wstring m_name;                              // lowercase repository folder name

    std::string Text(const TreeFile& f, UINT64 max = 1024 * 1024) {
        return f.size <= max ? ReadText(WorkFile(m_repo, f.path)) : "";
    }

    // The working file of an image in the tree, or empty.
    std::wstring Usable(const std::wstring& path) {
        if (path.empty()) return L"";
        auto it = m_index.find(Lower(path));
        if (it == m_index.end() || it->second->size == 0 || it->second->size > kMaxIconBytes) return L"";
        std::wstring ext = ExtOf(path);
        static const wchar_t* const exts[] = { L".ico", L".png", L".svg", L".xml", L".jpg", L".jpeg", L".bmp", L".gif", L".webp" };
        if (std::none_of(std::begin(exts), std::end(exts), [&](const wchar_t* e) { return ext == e; })) return L"";
        std::wstring file = WorkFile(m_repo, it->second->path);
        return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && IconShaped(file) ? file : L"";
    }

    // "<id> ICON [options] "file"": the first icon of the script is the executable's.
    std::wstring FromRc(const TreeFile& f) {
        static const std::regex icon(R"re(^\s*\w+\s+ICON\b[^"]*"([^"]+)")re", std::regex::icase);
        for (auto& line : Lines(Text(f))) {
            std::smatch m;
            if (!std::regex_search(line, m, icon)) continue;
            std::wstring ref = Widen(m[1]);
            for (size_t p; (p = ref.find(L"\\\\")) != std::wstring::npos;) ref.erase(p, 1);
            return Usable(JoinTree(DirOf(f.path), ref));
        }
        return L"";
    }

    std::wstring FromAndroid(const TreeFile& f) {
        static const std::regex icon(R"re(android:icon\s*=\s*"@(?:mipmap|drawable)/([\w.]+)")re");
        std::smatch m;
        std::string text = Text(f);
        return std::regex_search(text, m, icon) ? AndroidResource(DirOf(f.path) + L"res/", Widen(m[1])) : L"";
    }

    // An adaptive icon (mipmap-anydpi), else the densest bitmap, else any drawable of that name.
    std::wstring AndroidResource(const std::wstring& res, const std::wstring& name) {
        static const wchar_t* const densities[] = { L"xxxhdpi", L"xxhdpi", L"xhdpi", L"hdpi", L"mdpi" };
        std::wstring lowerRes = Lower(res), lowerName = Lower(name), best;
        int bestRank = INT_MAX;
        for (auto& f : m_files) {
            std::wstring lower = Lower(f.path);
            if (lower.compare(0, lowerRes.size(), lowerRes) != 0 || Depth(lower) != Depth(lowerRes) + 1) continue;
            std::wstring dir = lower.substr(lowerRes.size(), lower.find_last_of(L'/') - lowerRes.size()), file = NameOf(lower);
            std::wstring ext = ExtOf(file);
            if ((dir.compare(0, 6, L"mipmap") != 0 && dir.compare(0, 8, L"drawable") != 0) || file != lowerName + ext) continue;
            int rank;
            if (ext == L".xml") rank = dir.find(L"anydpi") != std::wstring::npos ? 0 : 20;
            else if (ext == L".png" || ext == L".webp") {
                rank = 10;
                for (int i = 0; i < ARRAYSIZE(densities); i++)
                    if (dir.find(densities[i]) != std::wstring::npos) { rank = 1 + i; break; }
            } else continue;
            if (rank < bestRank && !Usable(f.path).empty()) bestRank = rank, best = f.path;
        }
        return Usable(best);
    }

    // A path of a web page or manifest: relative to it, or from the site root ("/icon.png", tried in public/ too).
    std::wstring WebRef(const std::wstring& dir, std::string ref) {
        if (ref.find("://") != std::string::npos || ref.compare(0, 5, "data:") == 0 || ref.compare(0, 2, "//") == 0) return L"";
        ref = ref.substr(0, ref.find_first_of("?#"));
        std::wstring w = Widen(ref);
        if (!w.empty() && w[0] == L'/') {
            for (auto& base : { std::wstring(L"public/"), std::wstring(), dir }) {
                std::wstring icon = Usable(JoinTree(base, w.substr(1)));
                if (!icon.empty()) return icon;
            }
            return L"";
        }
        return Usable(JoinTree(dir, w));
    }

    // "icons": { "128": "icon128.png" } (browser extension) or [ { "src": ..., "sizes": "512x512" } ] (web app).
    std::wstring FromWebManifest(const TreeFile& f) {
        static const std::regex object(R"re("icons"\s*:\s*\{([^}]*)\})re"), pair(R"re("(\d+)"\s*:\s*"([^"]+)")re");
        static const std::regex list(R"re("icons"\s*:\s*\[([^\]]*)\])re"), item(R"re(\{[^}]*\})re");
        static const std::regex src(R"re("src"\s*:\s*"([^"]+)")re"), sizes(R"re("sizes"\s*:\s*"(\d+))re");
        std::string text = Text(f), best;
        int bestSize = -1;
        std::smatch m;
        if (std::regex_search(text, m, object)) {
            std::string body = m[1];
            for (std::sregex_iterator it(body.begin(), body.end(), pair), end; it != end; ++it)
                if (atoi((*it)[1].str().c_str()) > bestSize) bestSize = atoi((*it)[1].str().c_str()), best = (*it)[2];
        } else if (std::regex_search(text, m, list)) {
            std::string body = m[1];
            for (std::sregex_iterator it(body.begin(), body.end(), item), end; it != end; ++it) {
                std::string obj = (*it)[0];
                std::smatch s, z;
                if (!std::regex_search(obj, s, src)) continue;
                int size = std::regex_search(obj, z, sizes) ? atoi(z[1].str().c_str()) : 0;
                if (size > bestSize) bestSize = size, best = s[1];
            }
        }
        return best.empty() ? L"" : WebRef(DirOf(f.path), best);
    }

    std::wstring FromInstaller(const TreeFile& f) {
        static const std::regex setup(R"(^\s*SetupIconFile\s*=\s*(.+)$)", std::regex::icase);
        static const std::regex uninstall(R"(^\s*UninstallDisplayIcon\s*=\s*(.+)$)", std::regex::icase);
        static const std::regex nsis(R"re(^\s*(?:!define\s+MUI_ICON|Icon)\s+"([^"]+)")re", std::regex::icase);
        std::wstring dir = DirOf(f.path);
        for (auto& line : Lines(Text(f))) {
            std::smatch m;
            if ((std::regex_search(line, m, setup) || std::regex_search(line, m, nsis)) && m[1].str().find_first_of("{$") == std::string::npos) {
                std::wstring icon = Usable(JoinTree(dir, Widen(Unquote(m[1]))));
                if (!icon.empty()) return icon;
            } else if (std::regex_search(line, m, uninstall)) {
                // "{app}\share.ico": the installed copy of a file shipped from the script's folder.
                std::wstring value = Widen(Unquote(m[1]));
                value = value.substr(0, value.find(L','));
                std::wstring name = value.substr(value.find_last_of(L"\\/}") + 1);
                if (ExtOf(name) != L".exe") {
                    std::wstring icon = Usable(JoinTree(dir, name));
                    if (!icon.empty()) return icon;
                }
            }
        }
        return L"";
    }

    // A Loupedeck / Logi Actions plugin package: the marketplace icon sits beside its metadata file.
    std::wstring FromLoupedeck(const TreeFile& f) {
        return Usable(DirOf(f.path) + L"Icon256x256.png");
    }

    std::wstring FromHtml(const TreeFile& f) {
        static const std::regex link(R"(<link\b[^>]*>)", std::regex::icase);
        static const std::regex rel(R"re(\brel\s*=\s*["']([^"']*)["'])re", std::regex::icase);
        static const std::regex href(R"re(\bhref\s*=\s*["']([^"']+)["'])re", std::regex::icase);
        std::string text = Text(f, 8 * 1024 * 1024);
        for (std::sregex_iterator it(text.begin(), text.end(), link), end; it != end; ++it) {
            std::string tag = (*it)[0];
            std::smatch r, h;
            if (!std::regex_search(tag, r, rel) || Lower(Widen(r[1])).find(L"icon") == std::wstring::npos ||
                !std::regex_search(tag, h, href))
                continue;
            std::wstring icon = WebRef(DirOf(f.path), h[1]);
            if (!icon.empty()) return icon;
        }
        return L"";
    }

    // The first image of the README's first lines, when it is a file of the repository (a GitHub address included).
    std::wstring FromReadme(const TreeFile& f) {
        static const std::regex image(R"re(<img\b[^>]*\bsrc\s*=\s*["']([^"']+)["']|!\[[^\]]*\]\(\s*<?([^)\s>]+))re", std::regex::icase);
        if (Depth(f.path) != 0) return L"";
        for (auto& line : Lines(Text(f), 20)) {
            std::smatch m;
            if (!std::regex_search(line, m, image)) continue;
            std::wstring ref = Widen(m[1].matched ? m[1].str() : m[2].str());
            ref = ref.substr(0, ref.find_first_of(L"?#"));
            if (ExtOf(ref) == L".svg") return L"";
            size_t scheme = ref.find(L"://");
            if (scheme == std::wstring::npos) return Usable(JoinTree(L"", ref));
            if (Lower(ref).find(L"github") == std::wstring::npos) return L"";
            // https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<path>: the longest tail that is in the tree.
            std::wstring rest = ref.substr(scheme + 3);
            for (size_t slash = rest.find(L'/'); slash != std::wstring::npos; slash = rest.find(L'/', slash + 1)) {
                std::wstring icon = Usable(rest.substr(slash + 1));
                if (!icon.empty()) return icon;
            }
            return L"";
        }
        return L"";
    }

    std::wstring ByName() {
        int bestScore = 4;
        std::wstring best;
        for (auto& f : m_files) {
            int score = IconScore(f.path, f.size, m_name);
            if (score > bestScore && !Usable(f.path).empty()) bestScore = score, best = f.path;
        }
        return Usable(best);
    }

    static int IconScore(const std::wstring& path, UINT64 size, const std::wstring& repoName);
};

std::wstring FindIcon(const std::wstring& repo, const std::vector<TreeFile>& files) {
    return IconFinder(repo, files).Find();
}

int IconFinder::IconScore(const std::wstring& path, UINT64 size, const std::wstring& repoName) {
    std::wstring lower = Lower(path);
    size_t slash = lower.find_last_of(L'/');
    std::wstring name = lower.substr(slash == std::wstring::npos ? 0 : slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || size == 0 || size > kMaxIconBytes) return 0;
    std::wstring stem = name.substr(0, dot), ext = name.substr(dot);
    if (ext != L".ico" && ext != L".png" && ext != L".svg") return 0;
    int score = ext == L".png" ? 1 : 3;
    if (stem == L"icon" || stem == L"app" || stem == L"appicon" || stem == L"favicon" || stem == L"logo" ||
        stem == L"ic_launcher" || stem == repoName)
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

// The blobs of HEAD, else, on an unborn branch, the working files git would add (sizes read from the disk).
bool ListFiles(const std::wstring& repo, bool unborn, std::vector<TreeFile>& out) {
    std::string text;
    if (!GitStatus::Run(repo, unborn ? L"ls-files -z --cached --others --exclude-standard" : L"ls-tree -r -z -l HEAD", text))
        return false;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\0', pos);
        if (end == std::string::npos) end = text.size();
        std::string rec = text.substr(pos, end - pos);
        pos = end + 1;
        if (unborn) {
            WIN32_FILE_ATTRIBUTE_DATA data;
            std::wstring path = Widen(rec);
            if (path.empty() || !GetFileAttributesExW(WorkFile(repo, path).c_str(), GetFileExInfoStandard, &data) ||
                (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                continue;
            out.push_back({ path, ((UINT64)data.nFileSizeHigh << 32) | data.nFileSizeLow });
            continue;
        }
        // "<mode> <type> <object> <size>\t<path>"
        size_t tab = rec.find('\t');
        if (tab == std::string::npos || rec.compare(0, 6, "120000") == 0 || rec.find(" blob ") == std::string::npos) continue;
        size_t sizeStart = rec.find_last_of(' ', tab);
        out.push_back({ Widen(rec.substr(tab + 1)), _strtoui64(rec.c_str() + sizeStart + 1, nullptr, 10) });
    }
    return true;
}

RepoInfo Compute(const std::wstring& repo, bool unborn) {
    RepoInfo info;
    std::vector<TreeFile> tree;
    if (!ListFiles(repo, unborn, tree)) return info;

    struct Pending {
        std::vector<int> langs;
        UINT64 size;
    };
    std::vector<UINT64> bytes(ARRAYSIZE(kLinguistLanguages));
    std::vector<Pending> ambiguous;
    const Patterns& excluded = Excluded();
    std::vector<TreeFile> files;

    for (auto& t : tree) {
        const std::wstring& path = t.path;
        UINT64 size = t.size;
        size_t slash = path.find_last_of(L'/');
        std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);

        std::wstring file = WorkFile(repo, path);
        if (!excluded.Match(path)) files.push_back({ path, size });

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
    info.icon = FindIcon(repo, files);
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
            std::wstring commit = StateKey(repo);
            RepoInfo info = commit.empty() ? RepoInfo() : Compute(repo, commit.compare(0, wcslen(kUnborn), kUnborn) == 0);
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

    bool current = known && StateKey(repo) == commit;
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
