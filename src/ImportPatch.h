#pragma once
#include "Common.h"

// Redirects every import slot of `mod` matching `name` (or an ordinal from MAKEINTRESOURCEA)
// to `hook`, in both the normal and delay-load tables. `dll` restricts the match when not null.
int PatchImports(HMODULE mod, const char* dll, const char* name, void* hook);
