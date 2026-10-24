#include "Common.h"
#include <shlobj.h>
#include <cstdarg>
#include <cwchar>

static SRWLOCK s_logLock = SRWLOCK_INIT;

static std::wstring LogPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\ExplorerTint";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\ExplorerTint.log";
    }
    CoTaskMemFree(base);
    return path;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02u:%02u:%02u.%03u [%5lu] %s\r\n",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    OutputDebugStringW(line);

    static const std::wstring path = LogPath();
    if (path.empty()) return;
    char utf8[3600];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 1) return;
    AcquireSRWLockExclusive(&s_logLock);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(f, utf8, n - 1, &written, nullptr);
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&s_logLock);
}

bool IsDarkMode() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// uxtheme ordinal 133, undocumented, Windows 10 1809+.
void AllowDarkMode(HWND hwnd) {
    typedef BOOL (WINAPI* AllowDarkModeForWindow_t)(HWND, BOOL);
    static auto fn = (AllowDarkModeForWindow_t)GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(133));
    if (fn) fn(hwnd, TRUE);
}

bool UiFrench() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
