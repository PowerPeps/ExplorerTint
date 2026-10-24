#pragma once
#include "Settings.h"
#include <shobjidl.h>

void InstallFolderHooks(const Settings& settings);
bool ItemAttributes(IShellFolder* folder, PCUITEMID_CHILD child, DWORD& attrs);
bool IsSystemItem(DWORD attrs);

// Gives CompareIDs the sort direction of the view showing `folder`.
void RegisterSortView(IShellFolder* folder, IFolderView* view);
void UnregisterSortView(IShellFolder* folder);
