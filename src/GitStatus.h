#pragma once
#include "Common.h"

enum class GitState : BYTE { None, Staged, Untracked, Ignored };

namespace GitStatus {
    // Lowercase root of the repository holding folder, or empty. Starts loading its status.
    std::wstring RepoFor(const std::wstring& folder);
    GitState Query(const std::wstring& root, const std::wstring& relPath);
    void AddListener(HWND hwnd);
    void RemoveListener(HWND hwnd);
    void SetGitPath(const std::wstring& path);
    UINT RefreshMessage();
    // Runs git with the configured executable in root; out receives its standard output.
    bool Run(const std::wstring& root, const wchar_t* args, std::string& out);
}
