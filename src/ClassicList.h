#pragma once
#include "Common.h"

// Makes DefView fall back to SysListView32 by refusing the Itemsview COM class.
void InstallClassicList(bool enabled);
