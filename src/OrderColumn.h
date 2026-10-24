#pragma once
#include "Common.h"
#include <commctrl.h>
#include <shobjidl.h>

// The "#" column of a view: header label, rank values, and reordering by drag and drop.
struct OrderColumn;

OrderColumn* CreateOrderColumn(HWND list, IFolderView* view, const std::wstring& path);
void DestroyOrderColumn(OrderColumn* oc);

// Messages sent to the list. Returns true when `result` is set.
bool OrderColumnListMessage(OrderColumn* oc, HWND list, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
// LVN_GETDISPINFO, after DefView filled it.
void OrderColumnDispInfo(OrderColumn* oc, NMLVDISPINFOW* di);
void OrderColumnPostPaint(OrderColumn* oc, HDC hdc);
