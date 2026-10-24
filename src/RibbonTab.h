#pragma once
#include "Common.h"

// Adds the ExplorerTint tab to Explorer's ribbon. Hooks are installed once per process;
// `enabled` decides whether ribbons loaded afterwards get the tab.
void InstallRibbonTab(bool enabled);
