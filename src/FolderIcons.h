#pragma once
#include "Settings.h"

// System image list index of the icon the settings give to this folder, or -1 for the normal icon.
// Icons are built once into %LOCALAPPDATA%\ExplorerTint\icons.
int FolderIconIndex(const Settings& s, const std::wstring& parent, const WIN32_FIND_DATAW& fd);
int FolderIconIndex(const Settings& s, const std::wstring& path);

// Writes an .ico: folder (blue for a Git repository) with the badge in the bottom-right corner and, when bar is set,
// a language bar across the bottom. bar: "RRGGBB/percent,..."; a "ring:" badge draws the same as a ring.
// Badge: built-in name, "text" or "text"#RRGGBB, image file (.png, .jpg, .bmp, .gif) or icon location "file[,index]".
bool WriteFolderIcon(const std::wstring& file, bool repo, const std::wstring& badge, const std::wstring& bar = L"");

// Hue of Git repository folders, in degrees; IconTest uses it to compare variants.
void SetRepoHue(float hue);
