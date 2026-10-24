#pragma once
#include "Common.h"
#include <vector>

// Decodes an image file with WIC (PNG, JPEG, BMP, GIF, TIFF, WebP when its codec is installed): straight ARGB, top-down.
bool LoadRaster(const std::wstring& file, int& width, int& height, std::vector<UINT32>& argb);

// .svg files and Android drawables (.xml: vector, adaptive icon, shape, bitmap).
bool IsVector(const std::wstring& file);

// Renders a vector image into a size x size square (an adaptive icon as a rounded square): straight ARGB, top-down.
bool RenderVector(const std::wstring& file, int size, std::vector<UINT32>& argb);
