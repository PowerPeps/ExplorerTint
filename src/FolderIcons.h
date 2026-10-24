#pragma once
#include "Settings.h"

// System image list index of the icon the settings give to this folder, or -1 for the normal icon.
// Icons are built once into %LOCALAPPDATA%\ExplorerTint\icons.
int FolderIconIndex(const Settings& s, const std::wstring& parent, const WIN32_FIND_DATAW& fd);
int FolderIconIndex(const Settings& s, const std::wstring& path);

// Writes an .ico: folder (blue for a Git repository) with the badge in the bottom-right corner.
// Badge: built-in name, "text", image file (.png, .jpg, .bmp, .gif) or icon location "file[,index]".
bool WriteFolderIcon(const std::wstring& file, bool repo, const std::wstring& badge);
