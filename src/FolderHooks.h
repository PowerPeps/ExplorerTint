#pragma once
#include "Settings.h"
#include <shobjidl.h>

// Settings are read live through CurrentSettings().
void InstallFolderHooks();
bool ItemAttributes(IShellFolder* folder, PCUITEMID_CHILD child, DWORD& attrs);
bool IsSystemItem(DWORD attrs);

// Gives CompareIDs the sort direction of the view showing `folder`.
void RegisterSortView(IShellFolder* folder, IFolderView* view);
void UnregisterSortView(IShellFolder* folder);
