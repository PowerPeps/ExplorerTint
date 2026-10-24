#include "Settings.h"
#include <algorithm>
#include <cwctype>

static const wchar_t kKey[] = L"Software\\ExplorerTint";
static const wchar_t kDefaultHidden[] = L".claude;.bin;.build;.git;.github;.ia;.idea";
static const wchar_t kDefaultIcons[] =
    L"android=android;desktop=desktop;fonts=fonts;hardware=hardware;web=web;.git=git;.claude=claude;.kk=\"kk\"";
static const wchar_t kDefaultRoots[] = L"D:\\";

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static bool ReadDword(HKEY k, const wchar_t* name, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

static bool ReadString(HKEY k, const wchar_t* name, std::wstring& out) {
    DWORD size = 0;
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    size = (DWORD)(s.size() * sizeof(wchar_t));
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, &s[0], &size) != ERROR_SUCCESS) return false;
    s.resize(wcslen(s.c_str()));
    out = s;
    return true;
}

static void ReadColor(HKEY k, const wchar_t* name, Rgba& c) {
    std::wstring s;
    if (!ReadString(k, name, s)) return;
    Rgba parsed;
    if (ParseColor(s, parsed)) c = parsed;
    else Log(L"settings: invalid color %s = \"%s\"", name, s.c_str());
}

std::vector<std::wstring> SplitNames(const std::wstring& list) {
    std::vector<std::wstring> names;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();
        std::wstring name = list.substr(start, end - start);
        size_t a = name.find_first_not_of(L" \t"), b = name.find_last_not_of(L" \t");
        if (a != std::wstring::npos) {
            std::wstring lower = Lower(name.substr(a, b - a + 1));
            if (std::find(names.begin(), names.end(), lower) == names.end()) names.push_back(lower);
        }
        start = end + 1;
    }
    return names;
}

std::wstring JoinNames(const std::vector<std::wstring>& names) {
    std::wstring list;
    for (auto& n : names) list += (list.empty() ? L"" : L";") + n;
    return list;
}

// "name=badge;..." where a quoted badge may contain ';'.
std::vector<std::pair<std::wstring, std::wstring>> SplitIcons(const std::wstring& list) {
    std::vector<std::pair<std::wstring, std::wstring>> icons;
    std::vector<std::wstring> entries(1);
    bool quoted = false;
    for (wchar_t c : list) {
        if (c == L'"') quoted = !quoted;
        if (c == L';' && !quoted) entries.emplace_back();
        else entries.back() += c;
    }
    for (auto& e : entries) {
        size_t eq = e.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring name = Lower(Trim(e.substr(0, eq))), badge = Trim(e.substr(eq + 1));
        if (name.empty() || badge.empty()) continue;
        auto same = std::find_if(icons.begin(), icons.end(), [&](auto& p) { return p.first == name; });
        if (same == icons.end()) icons.emplace_back(name, badge);
    }
    return icons;
}

std::wstring JoinIcons(const std::vector<std::pair<std::wstring, std::wstring>>& icons, const wchar_t* separator) {
    std::wstring list;
    for (auto& p : icons) list += (list.empty() ? L"" : separator) + p.first + L"=" + p.second;
    return list;
}

std::vector<std::wstring> SplitRoots(const std::wstring& list) {
    std::vector<std::wstring> roots = SplitNames(list);
    for (auto& r : roots) {
        std::replace(r.begin(), r.end(), L'/', L'\\');
        if (r.size() == 1) r += L':';
        if (r.back() != L'\\') r += L'\\';
    }
    return roots;
}

bool ParseColor(const std::wstring& s, Rgba& out) {
    out = Rgba();
    if (s.empty()) return true;
    if (s[0] != L'#' || (s.size() != 7 && s.size() != 9)) return false;
    BYTE v[4] = { 0, 0, 0, 0xFF };
    for (size_t i = 1; i + 1 < s.size(); i += 2) {
        if (!iswxdigit(s[i]) || !iswxdigit(s[i + 1])) return false;
        wchar_t hex[3] = { s[i], s[i + 1], 0 };
        v[i / 2] = (BYTE)wcstoul(hex, nullptr, 16);
    }
    out.r = v[0];
    out.g = v[1];
    out.b = v[2];
    out.a = v[3];
    out.set = true;
    return true;
}

std::wstring FormatColor(const Rgba& c) {
    if (!c.set) return L"";
    wchar_t buf[16];
    if (c.a == 0xFF) swprintf_s(buf, L"#%02X%02X%02X", c.r, c.g, c.b);
    else swprintf_s(buf, L"#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return buf;
}

COLORREF Blend(const Rgba& c, COLORREF under) {
    auto mix = [&](BYTE top, BYTE bottom) {
        return (BYTE)((top * c.a + bottom * (255 - c.a) + 127) / 255);
    };
    return RGB(mix(c.r, GetRValue(under)), mix(c.g, GetGValue(under)), mix(c.b, GetBValue(under)));
}

Settings DefaultSettings() {
    Settings s;
    ParseColor(L"#C5783D", s.untracked.text);
    ParseColor(L"#C5783D", s.ignored.text);
    ParseColor(L"#9B49301F", s.ignored.back);
    ParseColor(L"#80808099", s.system.text);
    s.hiddenNames = SplitNames(kDefaultHidden);
    s.folderIcons = SplitIcons(kDefaultIcons);
    s.iconRoots = SplitRoots(kDefaultRoots);
    return s;
}

Settings LoadSettings() {
    Settings s = DefaultSettings();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return s;
    DWORD d;
    if (ReadDword(k, L"ClassicList", d)) s.classicList = d != 0;
    if (ReadDword(k, L"Git", d)) s.git = d != 0;
    ReadColor(k, L"StagedText", s.staged.text);
    ReadColor(k, L"StagedBack", s.staged.back);
    ReadColor(k, L"UntrackedText", s.untracked.text);
    ReadColor(k, L"UntrackedBack", s.untracked.back);
    ReadColor(k, L"IgnoredText", s.ignored.text);
    ReadColor(k, L"IgnoredBack", s.ignored.back);
    ReadString(k, L"GitPath", s.gitPath);
    ReadColor(k, L"SystemText", s.system.text);
    ReadColor(k, L"SystemBack", s.system.back);
    if (ReadDword(k, L"SystemLast", d)) s.systemLast = d != 0;
    if (ReadDword(k, L"HideNames", d)) s.hideNames = d != 0;
    std::wstring hidden;
    if (ReadString(k, L"HiddenNames", hidden)) s.hiddenNames = SplitNames(hidden);
    if (ReadDword(k, L"RibbonTab", d)) s.ribbonTab = d != 0;
    if (ReadDword(k, L"RepoIcon", d)) s.repoIcon = d != 0;
    std::wstring list;
    if (ReadString(k, L"FolderIcons", list)) s.folderIcons = SplitIcons(list);
    if (ReadString(k, L"IconRoots", list)) s.iconRoots = SplitRoots(list);
    RegCloseKey(k);
    return s;
}

static LONG WriteDword(HKEY k, const wchar_t* name, DWORD value) {
    return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
}

static LONG WriteString(HKEY k, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

bool SaveSettings(const Settings& s) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG e = WriteDword(k, L"ClassicList", s.classicList);
    if (!e) e = WriteDword(k, L"Git", s.git);
    if (!e) e = WriteString(k, L"StagedText", FormatColor(s.staged.text));
    if (!e) e = WriteString(k, L"StagedBack", FormatColor(s.staged.back));
    if (!e) e = WriteString(k, L"UntrackedText", FormatColor(s.untracked.text));
    if (!e) e = WriteString(k, L"UntrackedBack", FormatColor(s.untracked.back));
    if (!e) e = WriteString(k, L"IgnoredText", FormatColor(s.ignored.text));
    if (!e) e = WriteString(k, L"IgnoredBack", FormatColor(s.ignored.back));
    if (!e) e = WriteString(k, L"GitPath", s.gitPath);
    if (!e) e = WriteString(k, L"SystemText", FormatColor(s.system.text));
    if (!e) e = WriteString(k, L"SystemBack", FormatColor(s.system.back));
    if (!e) e = WriteDword(k, L"SystemLast", s.systemLast);
    if (!e) e = WriteDword(k, L"HideNames", s.hideNames);
    if (!e) e = WriteString(k, L"HiddenNames", JoinNames(s.hiddenNames));
    if (!e) e = WriteDword(k, L"RibbonTab", s.ribbonTab);
    if (!e) e = WriteDword(k, L"RepoIcon", s.repoIcon);
    if (!e) e = WriteString(k, L"FolderIcons", JoinIcons(s.folderIcons));
    if (!e) e = WriteString(k, L"IconRoots", JoinNames(s.iconRoots));
    RegCloseKey(k);
    if (e) Log(L"settings: save failed (%ld)", e);
    return e == ERROR_SUCCESS;
}

bool SaveSetting(const wchar_t* name, DWORD value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG e = WriteDword(k, name, value);
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

namespace {

INIT_ONCE s_watchOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK s_lock = SRWLOCK_INIT;
std::shared_ptr<const Settings> s_current;
std::vector<HWND> s_listeners;
HKEY s_watchKey = nullptr;
HANDLE s_watchEvent = nullptr;
HANDLE s_wait = nullptr;
UINT s_message = 0;

bool Arm() {
    const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
    return RegNotifyChangeKeyValue(s_watchKey, TRUE, filter, s_watchEvent, TRUE) == ERROR_SUCCESS;
}

void CALLBACK OnKeyChanged(void*, BOOLEAN) {
    Sleep(100);  // lets a burst of writes land before reloading once
    Arm();
    auto fresh = std::make_shared<const Settings>(LoadSettings());
    AcquireSRWLockExclusive(&s_lock);
    s_current = fresh;
    std::vector<HWND> listeners = s_listeners;
    ReleaseSRWLockExclusive(&s_lock);
    Log(L"settings: reloaded, %zu listener(s)", listeners.size());
    for (HWND h : listeners) PostMessageW(h, s_message, 0, 0);
}

BOOL CALLBACK StartWatch(PINIT_ONCE, void*, void**) {
    s_message = RegisterWindowMessageW(L"ExplorerTint.SettingsChanged");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &s_watchKey, nullptr) !=
        ERROR_SUCCESS) {
        Log(L"settings: key not opened for watching");
        return TRUE;
    }
    s_watchEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_watchEvent || !Arm() ||
        !RegisterWaitForSingleObject(&s_wait, s_watchEvent, OnKeyChanged, nullptr, INFINITE, WT_EXECUTEDEFAULT))
        Log(L"settings: watch not started");
    return TRUE;
}

}

void WatchSettings() {
    InitOnceExecuteOnce(&s_watchOnce, StartWatch, nullptr, nullptr);
}

std::shared_ptr<const Settings> CurrentSettings() {
    AcquireSRWLockShared(&s_lock);
    auto s = s_current;
    ReleaseSRWLockShared(&s_lock);
    if (s) return s;
    auto fresh = std::make_shared<const Settings>(LoadSettings());
    AcquireSRWLockExclusive(&s_lock);
    if (!s_current) s_current = fresh;
    s = s_current;
    ReleaseSRWLockExclusive(&s_lock);
    return s;
}

UINT SettingsChangedMessage() {
    WatchSettings();
    return s_message;
}

void AddSettingsListener(HWND hwnd) {
    AcquireSRWLockExclusive(&s_lock);
    if (std::find(s_listeners.begin(), s_listeners.end(), hwnd) == s_listeners.end()) s_listeners.push_back(hwnd);
    ReleaseSRWLockExclusive(&s_lock);
}

void RemoveSettingsListener(HWND hwnd) {
    AcquireSRWLockExclusive(&s_lock);
    s_listeners.erase(std::remove(s_listeners.begin(), s_listeners.end(), hwnd), s_listeners.end());
    ReleaseSRWLockExclusive(&s_lock);
}
