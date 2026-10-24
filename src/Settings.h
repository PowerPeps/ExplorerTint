#pragma once
#include "Common.h"
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
    std::vector<std::wstring> hiddenNames;  // lowercase
    bool ribbonTab = true;
};

Settings LoadSettings();
bool ParseColor(const std::wstring& s, Rgba& out);
COLORREF Blend(const Rgba& over, COLORREF under);
