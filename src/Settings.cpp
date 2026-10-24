#include "Settings.h"
#include <cwctype>

static const wchar_t kKey[] = L"Software\\ExplorerTint";

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

static std::vector<std::wstring> SplitNames(const std::wstring& list) {
    std::vector<std::wstring> names;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();
        std::wstring name = list.substr(start, end - start);
        size_t a = name.find_first_not_of(L" \t"), b = name.find_last_not_of(L" \t");
        if (a != std::wstring::npos) names.push_back(Lower(name.substr(a, b - a + 1)));
        start = end + 1;
    }
    return names;
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

COLORREF Blend(const Rgba& c, COLORREF under) {
    auto mix = [&](BYTE top, BYTE bottom) {
        return (BYTE)((top * c.a + bottom * (255 - c.a) + 127) / 255);
    };
    return RGB(mix(c.r, GetRValue(under)), mix(c.g, GetGValue(under)), mix(c.b, GetBValue(under)));
}

Settings LoadSettings() {
    Settings s;
    ParseColor(L"#C5783D", s.untracked.text);
    ParseColor(L"#C5783D", s.ignored.text);
    ParseColor(L"#9B49301F", s.ignored.back);
    ParseColor(L"#80808099", s.system.text);
    std::wstring hidden = L".claude;.bin;.build;.git;.github;.ia;.idea";

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        s.hiddenNames = SplitNames(hidden);
        return s;
    }
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
    ReadString(k, L"HiddenNames", hidden);
    if (ReadDword(k, L"RibbonTab", d)) s.ribbonTab = d != 0;
    RegCloseKey(k);
    s.hiddenNames = SplitNames(hidden);
    return s;
}
