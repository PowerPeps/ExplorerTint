// Writes every default folder icon, normal and Git repository variants, into a directory.
// Usage: IconTest.exe <directory> [badge ...]
#include "../src/FolderIcons.h"
#include <cstdio>

HINSTANCE g_hinst = GetModuleHandleW(nullptr);

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: IconTest <directory> [badge ...]\n");
        return 2;
    }
    std::wstring dir = argv[1];
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> badges;
    for (int i = 2; i < argc; i++) badges.push_back(argv[i]);
    if (badges.empty()) {
        badges.push_back(L"");
        for (auto& p : DefaultSettings().folderIcons) badges.push_back(p.second);
    }
    int failures = 0;
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
