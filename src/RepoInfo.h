#pragma once
#include "Common.h"
#include <vector>

struct LanguageShare {
    std::wstring name;
    UINT32 color;  // 0xRRGGBB
    double share;  // 0..1
};

struct RepoInfo {
    std::vector<LanguageShare> languages;  // largest first
    std::wstring icon;                     // the project's own icon file, or empty
};

// Languages of the repository's HEAD as GitHub's linguist counts them (programming and markup files,
// vendored and documentation paths excluded), and the project's icon (favicon, app icon, logo).
// Returns false until they are first computed on a worker thread, which then tells Explorer to refresh the folder;
// a new commit gives the previous values until the new ones are ready.
bool GetRepoInfo(const std::wstring& repo, RepoInfo& out);
