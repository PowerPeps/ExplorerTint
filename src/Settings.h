#pragma once
#include "Common.h"
#include <memory>
#include <vector>

struct Rgba {
    BYTE r = 0, g = 0, b = 0, a = 0;
    bool set = false;
};

struct StateStyle {
    Rgba text;
    Rgba back;
};

struct Settings {
    bool classicList = true;
    bool git = true;
    StateStyle staged;
    StateStyle untracked;
    StateStyle ignored;
    std::wstring gitPath;
    StateStyle system;
    bool systemLast = true;
    bool hideNames = true;
    std::vector<std::wstring> hiddenNames;  // lowercase
    bool ribbonTab = true;
    bool repoIcon = true;
    std::wstring repoBadge = L"ring";                                  // lowercase: icon, ring, bar, combined
    std::vector<std::pair<std::wstring, std::wstring>> folderIcons;  // (lowercase name pattern, badge)
    std::vector<std::wstring> iconRoots;                              // lowercase, ending with '\'
};

Settings LoadSettings();
Settings DefaultSettings();
bool SaveSettings(const Settings& s);
bool SaveSetting(const wchar_t* name, DWORD value);

// Live settings, reloaded when HKCU\Software\ExplorerTint changes. Listeners receive
// SettingsChangedMessage() on their own thread.
void WatchSettings();
std::shared_ptr<const Settings> CurrentSettings();
UINT SettingsChangedMessage();
void AddSettingsListener(HWND hwnd);
void RemoveSettingsListener(HWND hwnd);

std::wstring FormatColor(const Rgba& c);
std::wstring JoinNames(const std::vector<std::wstring>& names);
std::vector<std::wstring> SplitNames(const std::wstring& list);
std::wstring JoinIcons(const std::vector<std::pair<std::wstring, std::wstring>>& icons, const wchar_t* separator = L";");
std::vector<std::pair<std::wstring, std::wstring>> SplitIcons(const std::wstring& list);
std::vector<std::wstring> SplitRoots(const std::wstring& list);
bool ParseColor(const std::wstring& s, Rgba& out);
COLORREF Blend(const Rgba& over, COLORREF under);
