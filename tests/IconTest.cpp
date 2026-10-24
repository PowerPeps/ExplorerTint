// Writes every default folder icon, normal and Git repository variants, into a directory.
// Usage: IconTest.exe <directory> [--hue degrees] [badge ...]
//        IconTest.exe <directory> --repo <repository> ...: languages and project icon, then ring, bar and icon variants.
#include "../src/FolderIcons.h"
#include "../src/RepoInfo.h"
#include <cstdio>

HINSTANCE g_hinst = GetModuleHandleW(nullptr);

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: IconTest <directory> [--hue degrees] [badge ...]\n");
        return 2;
    }
    std::wstring dir = argv[1];
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> badges;
    for (int i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--hue") && i + 1 < argc) SetRepoHue((float)_wtof(argv[++i]));
        else badges.push_back(argv[i]);
    }
    std::vector<std::wstring> repos;
    for (auto it = badges.begin(); it != badges.end();) {
        if (*it == L"--repo" && it + 1 != badges.end()) {
            repos.push_back(*(it + 1));
            it = badges.erase(it, it + 2);
        } else {
            ++it;
        }
    }
    int failures = 0;
    for (auto& repo : repos) {
        RepoInfo info;
        for (int i = 0; i < 600 && !GetRepoInfo(repo, info); i++) Sleep(100);
        wprintf(L"%s\n  icon: %s\n", repo.c_str(), info.icon.c_str());
        std::wstring segments;
        for (auto& l : info.languages) {
            wprintf(L"  %-20s #%06X %5.1f %%\n", l.name.c_str(), l.color, l.share * 100);
            wchar_t item[24];
            swprintf_s(item, L"%s%06X/%d", segments.empty() ? L"" : L",", l.color, (int)(l.share * 100 + 0.5));
            if (l.share >= 0.005) segments += item;
        }
        std::wstring name = repo.substr(repo.find_last_of(L'\\') + 1);
        struct { const wchar_t* style; std::wstring badge, bar; } variants[] = {
            { L"ring", L"ring:" + segments, L"" }, { L"bar", L"", segments }, { L"icon", info.icon, L"" },
        };
        for (auto& v : variants) {
            if (v.badge.empty() && v.bar.empty()) continue;
            std::wstring file = dir + L"\\" + name + L"-" + v.style + L".ico";
            bool ok = WriteFolderIcon(file, true, v.badge, v.bar);
            wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", file.c_str());
            failures += !ok;
        }
    }
    if (!repos.empty()) return failures ? 1 : 0;
    if (badges.empty()) {
        badges.push_back(L"");
        for (auto& p : DefaultSettings().folderIcons) badges.push_back(p.second);
    }
    for (auto& badge : badges)
        for (bool repo : { false, true }) {
            std::wstring name = badge.empty() ? L"plain" : badge;
            for (auto& c : name)
                if (wcschr(L"\"\\/:*?<>|,", c)) c = L'_';
            std::wstring file = dir + L"\\" + name + (repo ? L"-repo" : L"") + L".ico";
            bool ok = WriteFolderIcon(file, repo, badge);
            wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", file.c_str());
            failures += !ok;
        }
    wprintf(failures ? L"%d failure(s)\n" : L"OK (0 failures)\n", failures);
    return failures ? 1 : 0;
}
