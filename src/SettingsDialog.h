#pragma once
#include "Common.h"

// Modal settings dialog; saving writes HKCU\Software\ExplorerTint, which every Explorer process watches.
void ShowSettingsDialog(HWND owner);
