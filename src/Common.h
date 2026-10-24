#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

extern HINSTANCE g_hinst;

void Log(const wchar_t* fmt, ...);
std::wstring Lower(std::wstring s);
bool IsDarkMode();
void AllowDarkMode(HWND hwnd);
bool UiFrench();
