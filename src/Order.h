#pragma once
#include "Common.h"
#include <vector>
#include <propsys.h>

// Custom item order per folder, stored in %LOCALAPPDATA%\ExplorerTint\order.ini.
namespace Order {

int Rank(const std::wstring& folder, const std::wstring& name);  // 1-based, 0 when unranked
bool Has(const std::wstring& folder);
void Set(const std::wstring& folder, const std::vector<std::wstring>& names);

// Existing shell property whose column is shown as "#".
const PROPERTYKEY& HostKey();
const std::wstring& HostLabel();

}
