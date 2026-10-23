#pragma once
#include "Common.h"
#include <unknwn.h>

extern const CLSID CLSID_ExplorerTintBho;
extern LONG g_cRefModule;

IUnknown* CreateBho();
