#include "FolderIcons.h"
#include "resource.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

const int kSizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
const int kVersion = 1;  // part of the cached file names: bump when the drawing changes
const BYTE kHaloAlpha = 120;

struct Pixels {
    int size = 0;
    std::vector<UINT32> px;  // 0xAARRGGBB, straight alpha, top-down
};

INIT_ONCE s_gdiplusOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK s_cacheLock = SRWLOCK_INIT;
std::map<std::wstring, int> s_cache;

BOOL CALLBACK StartGdiplus(PINIT_ONCE, void*, void**) {
    GdiplusStartupInput input;
    ULONG_PTR token;
    return GdiplusStartup(&token, &input, nullptr) == Ok;
}

bool UseGdiplus() {
    return InitOnceExecuteOnce(&s_gdiplusOnce, StartGdiplus, nullptr, nullptr) != FALSE;
}

bool FromIcon(HICON icon, Pixels& out) {
    ICONINFO ii;
    if (!icon || !GetIconInfo(icon, &ii)) return false;
    BITMAP bm = {};
    bool ok = ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth == bm.bmHeight && bm.bmWidth > 0;
    if (ok) {
        int n = bm.bmWidth;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = n;
        bi.bmiHeader.biHeight = -n;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.size = n;
        out.px.assign(n * n, 0);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, ii.hbmColor, 0, n, out.px.data(), &bi, DIB_RGB_COLORS) == n;
        if (ok && std::none_of(out.px.begin(), out.px.end(), [](UINT32 c) { return (c >> 24) != 0; })) {
            std::vector<UINT32> mask(n * n);
            ok = GetDIBits(dc, ii.hbmMask, 0, n, mask.data(), &bi, DIB_RGB_COLORS) == n;
            for (size_t i = 0; ok && i < mask.size(); i++)
                out.px[i] = (out.px[i] & 0xFFFFFF) | ((mask[i] & 0xFFFFFF) ? 0 : 0xFF000000);
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

bool FromBitmap(Bitmap& bmp, Pixels& out) {
    int n = (int)bmp.GetWidth();
    if (!n || (int)bmp.GetHeight() != n) return false;
    out.size = n;
    out.px.assign(n * n, 0);
    BitmapData bd = {};
    bd.Width = n;
    bd.Height = n;
    bd.Stride = n * 4;
    bd.PixelFormat = PixelFormat32bppARGB;
    bd.Scan0 = out.px.data();
    Rect rc(0, 0, n, n);
    if (bmp.LockBits(&rc, ImageLockModeRead | ImageLockModeUserInputBuf, PixelFormat32bppARGB, &bd) != Ok) return false;
    bmp.UnlockBits(&bd);
    return true;
}

Pixels Scale(const Pixels& src, int size) {
    if (src.size == size) return src;
    Pixels out;
    out.size = size;
    out.px.assign(size * size, 0);
    Bitmap from(src.size, src.size, src.size * 4, PixelFormat32bppARGB, (BYTE*)src.px.data());
    Bitmap to(size, size, size * 4, PixelFormat32bppARGB, (BYTE*)out.px.data());
    Graphics g(&to);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes wrap;
    wrap.SetWrapMode(WrapModeTileFlipXY);
    g.DrawImage(&from, Rect(0, 0, size, size), 0, 0, src.size, src.size, UnitPixel, &wrap);
    g.Flush(FlushIntentionSync);
    return out;
}

bool ResourcePixels(int id, Pixels& out) {
    HRSRC r = FindResourceW(g_hinst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL h = r ? LoadResource(g_hinst, r) : nullptr;
    const BYTE* data = h ? (const BYTE*)LockResource(h) : nullptr;
    IStream* stream = data ? SHCreateMemStream(data, SizeofResource(g_hinst, r)) : nullptr;
    if (!stream) return false;
    bool ok = false;
    {
        Bitmap bmp(stream);
        ok = bmp.GetLastStatus() == Ok && FromBitmap(bmp, out);
    }
    stream->Release();
    return ok;
}

std::vector<Pixels> LoadRepoSources() {
    std::vector<Pixels> sources;
    for (int id : { IDR_REPO16, IDR_REPO32, IDR_REPO48, IDR_REPO256 }) {
        Pixels p;
        if (ResourcePixels(id, p)) sources.push_back(std::move(p));
    }
    return sources;
}

// Exact size, else twice the size, else the smallest larger one.
bool RepoBase(int size, Pixels& out) {
    static const std::vector<Pixels> sources = LoadRepoSources();
    const Pixels* best = nullptr;
    for (auto& s : sources)
        if (s.size == size) best = &s;
    for (auto& s : sources)
        if (!best && s.size == 2 * size) best = &s;
    for (auto& s : sources)
        if (s.size > size && (!best || (best->size != size && best->size != 2 * size && s.size < best->size))) best = &s;
    if (!best && !sources.empty()) best = &sources.back();
    if (!best) return false;
    out = Scale(*best, size);
    return true;
}

bool FolderBase(int size, Pixels& out) {
    SHSTOCKICONINFO sii = { sizeof(sii) };
    if (FAILED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &sii))) return false;
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(sii.szPath, sii.iIcon, 0, &icon, nullptr, size)) || !icon) return false;
    Pixels native;
    bool ok = FromIcon(icon, native);
    DestroyIcon(icon);
    if (ok) out = Scale(native, size);
    return ok;
}

// Badges are drawn in a unit square; px is the size of one pixel in those units.

void RoundRect(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Graphics& g, const Brush& brush, REAL x, REAL y, REAL w, REAL h, REAL r) {
    GraphicsPath path;
    RoundRect(path, x, y, w, h, r);
    g.FillPath(&brush, &path);
}

// Scales the path into the unit square: centred, on the bottom edge.
void Fit(GraphicsPath& path, REAL w, REAL h) {
    RectF b;
    path.GetBounds(&b);
    if (b.Width <= 0 || b.Height <= 0) return;
    REAL k = std::min(w / b.Width, h / b.Height);
    Matrix m;
    m.Translate((1 - b.Width * k) / 2, 1 - b.Height * k);
    m.Scale(k, k);
    m.Translate(-b.X, -b.Y);
    path.Transform(&m);
}

void Glyphs(Graphics& g, const wchar_t* text, const wchar_t* family, int style, REAL w, REAL h, const Color& color) {
    FontFamily ff(family);
    if (ff.GetLastStatus() != Ok) return;
    GraphicsPath path;
    path.AddString(text, (INT)wcslen(text), &ff, style, 1.0f, PointF(0, 0), StringFormat::GenericTypographic());
    Fit(path, w, h);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void Git(Graphics& g, REAL px) {
    GraphicsPath diamond;
    REAL side = 0.98f / 1.41421356f;
    RoundRect(diamond, -side / 2, -side / 2, side, side, 0.07f);
    Matrix m;
    m.Translate(0.5f, 0.5f);
    m.Rotate(45);
    diamond.Transform(&m);
    SolidBrush orange(Color(0xF0, 0x50, 0x33));
    g.FillPath(&orange, &diamond);

    Color white(0xFF, 0xFF, 0xFF);
    Pen line(white, std::max(0.065f, px));
    line.SetStartCap(LineCapRound);
    line.SetEndCap(LineCapRound);
    SolidBrush dot(white);
    const PointF join(0.49f, 0.30f), bottom(0.49f, 0.71f), right(0.71f, 0.51f);
    const REAL r = std::max(0.085f, 1.1f * px);
    g.SetClip(&diamond);
    g.DrawLine(&line, PointF(0.30f, 0.05f), join);
    g.ResetClip();
    g.DrawLine(&line, join, bottom);
    g.DrawLine(&line, join, right);
    for (auto& p : { join, bottom, right }) g.FillEllipse(&dot, p.X - r, p.Y - r, 2 * r, 2 * r);
}

void Android(Graphics& g, REAL px) {
    SolidBrush green(Color(0x3D, 0xDC, 0x84));
    Pen antenna(Color(0x3D, 0xDC, 0x84), std::max(0.075f, px));
    antenna.SetStartCap(LineCapRound);
    antenna.SetEndCap(LineCapRound);
    g.DrawLine(&antenna, 0.31f, 0.62f, 0.17f, 0.36f);
    g.DrawLine(&antenna, 0.69f, 0.62f, 0.83f, 0.36f);
    g.FillPie(&green, RectF(0.02f, 0.48f, 0.96f, 0.96f), 180, 180);
    SolidBrush eye(Color(0x07, 0x30, 0x42));
    REAL r = std::max(0.055f, 0.6f * px);
    g.FillEllipse(&eye, 0.31f - r, 0.80f - r, 2 * r, 2 * r);
    g.FillEllipse(&eye, 0.69f - r, 0.80f - r, 2 * r, 2 * r);
}

void Desktop(Graphics& g, REAL px) {
    SolidBrush frame(Color(0x50, 0x50, 0x58));
    LinearGradientBrush screen(PointF(0, 0.15f), PointF(0, 0.70f), Color(0x5C, 0xC8, 0xFF), Color(0x00, 0x5F, 0xB8));
    FillRound(g, frame, 0.02f, 0.10f, 0.96f, 0.64f, 0.07f);
    REAL bezel = std::max(0.07f, px);
    g.FillRectangle(&screen, 0.02f + bezel, 0.10f + bezel, 0.96f - 2 * bezel, 0.64f - 2 * bezel);
    g.FillRectangle(&frame, 0.42f, 0.72f, 0.16f, 0.14f);
    FillRound(g, frame, 0.22f, 0.84f, 0.56f, 0.12f, 0.04f);
}

void Fonts(Graphics& g, REAL px) {
    bool tiny = px > 1.0f / 20;
    Glyphs(g, L"A", L"Times New Roman", tiny ? FontStyleBold : FontStyleRegular, 1.0f, 0.95f, Color(0x00, 0x78, 0xD7));
}

void Hardware(Graphics& g, REAL px) {
    SolidBrush pin(Color(0xE0, 0xB6, 0x4A)), body(Color(0x2E, 0x38, 0x42)), die(Color(0x6B, 0x7F, 0x8E));
    REAL w = std::max(0.09f, px);
    for (REAL c : { 0.30f, 0.50f, 0.70f }) {
        g.FillRectangle(&pin, c - w / 2, 0.04f, w, 0.92f);
        g.FillRectangle(&pin, 0.04f, c - w / 2, 0.92f, w);
    }
    FillRound(g, body, 0.17f, 0.17f, 0.66f, 0.66f, 0.07f);
    FillRound(g, die, 0.33f, 0.33f, 0.34f, 0.34f, 0.04f);
}

void Web(Graphics& g, REAL px) {
    GraphicsPath globe;
    globe.AddEllipse(0.03f, 0.03f, 0.94f, 0.94f);
    LinearGradientBrush blue(PointF(0.1f, 0.1f), PointF(0.9f, 0.9f), Color(0x56, 0xB8, 0xF8), Color(0x12, 0x5E, 0xC0));
    g.FillPath(&blue, &globe);
    Pen line(Color(0xF0, 0xFF, 0xFF, 0xFF), std::max(0.06f, 0.9f * px));
    g.SetClip(&globe);
    g.DrawEllipse(&line, 0.29f, 0.03f, 0.42f, 0.94f);
    g.DrawLine(&line, 0.03f, 0.50f, 0.97f, 0.50f);
    if (px < 1.0f / 14) {
        g.DrawLine(&line, 0.50f, 0.03f, 0.50f, 0.97f);
        g.DrawLine(&line, 0.03f, 0.27f, 0.97f, 0.27f);
        g.DrawLine(&line, 0.03f, 0.73f, 0.97f, 0.73f);
    }
    g.ResetClip();
}

void Claude(Graphics& g, REAL px) {
    static const REAL len[] = { 0.47f, 0.37f, 0.44f, 0.36f, 0.46f, 0.39f, 0.47f, 0.36f, 0.43f, 0.38f, 0.46f, 0.37f };
    static const REAL jitter[] = { 0, 5, -4, 3, -2, 6, 0, -5, 4, -3, 2, -6 };
    SolidBrush coral(Color(0xD9, 0x77, 0x57));
    const REAL w0 = std::max(0.075f, px), w1 = std::max(0.042f, 0.6f * px);
    for (int i = 0; i < 12; i++) {
        REAL a = (i * 30.0f + jitter[i] - 90.0f) * 3.14159265f / 180.0f;
        REAL dx = cosf(a), dy = sinf(a), l = len[i];
        PointF pts[4] = {
            { 0.5f - dy * w0, 0.5f + dx * w0 },
            { 0.5f + dx * l - dy * w1, 0.5f + dy * l + dx * w1 },
            { 0.5f + dx * l + dy * w1, 0.5f + dy * l - dx * w1 },
            { 0.5f + dy * w0, 0.5f - dx * w0 },
        };
        g.FillPolygon(&coral, pts, 4);
        g.FillEllipse(&coral, 0.5f + dx * l - w1, 0.5f + dy * l - w1, 2 * w1, 2 * w1);
    }
    g.FillEllipse(&coral, 0.38f, 0.38f, 0.24f, 0.24f);
}

void Text(Graphics& g, const std::wstring& text) {
    Glyphs(g, text.c_str(), L"Segoe UI", FontStyleBold, 1.0f, 0.80f, Color(0x00, 0x78, 0xD7));
}

bool IsRaster(const std::wstring& file) {
    const wchar_t* ext = PathFindExtensionW(file.c_str());
    for (auto e : { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff" })
        if (!_wcsicmp(ext, e)) return true;
    return false;
}

// "file" or "file,index"; empty when the file does not exist.
std::wstring BadgeFile(const std::wstring& badge, int& index) {
    wchar_t expanded[MAX_PATH];
    if (!ExpandEnvironmentStringsW(badge.c_str(), expanded, ARRAYSIZE(expanded))) return L"";
    std::wstring file = expanded;
    index = 0;
    size_t comma = file.rfind(L',');
    if (comma != std::wstring::npos && comma > file.find_last_of(L"\\/") + 1) {
        wchar_t* end = nullptr;
        long n = wcstol(file.c_str() + comma + 1, &end, 10);
        if (end && !*end) {
            index = (int)n;
            file.resize(comma);
        }
    }
    if (file.find_first_of(L"\\/") == std::wstring::npos) return L"";
    DWORD attrs = GetFileAttributesW(file.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? file : L"";
}

void DrawFitted(Graphics& g, Bitmap& image) {
    REAL w = (REAL)image.GetWidth(), h = (REAL)image.GetHeight();
    if (w <= 0 || h <= 0) return;
    REAL k = std::min(1 / w, 1 / h);
    g.DrawImage(&image, RectF((1 - w * k) / 2, 1 - h * k, w * k, h * k));
}

bool DrawFile(Graphics& g, const std::wstring& file, int index, int pixels) {
    if (IsRaster(file)) {
        Bitmap image(file.c_str());
        if (image.GetLastStatus() != Ok) return false;
        DrawFitted(g, image);
        return true;
    }
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, pixels)) || !icon) return false;
    Pixels p;
    bool ok = FromIcon(icon, p);
    DestroyIcon(icon);
    if (!ok) return false;
    Bitmap image(p.size, p.size, p.size * 4, PixelFormat32bppARGB, (BYTE*)p.px.data());
    DrawFitted(g, image);
    return true;
}

typedef void (*Builtin)(Graphics&, REAL);

Builtin FindBuiltin(const std::wstring& badge) {
    static const struct {
        const wchar_t* name;
        Builtin draw;
    } kBuiltins[] = {
        { L"android", Android }, { L"claude", Claude }, { L"desktop", Desktop }, { L"fonts", Fonts },
        { L"git", Git },         { L"hardware", Hardware }, { L"web", Web },
    };
    for (auto& b : kBuiltins)
        if (!_wcsicmp(badge.c_str(), b.name)) return b.draw;
    return nullptr;
}

bool Quoted(const std::wstring& badge) {
    return badge.size() >= 2 && badge.front() == L'"' && badge.back() == L'"';
}

bool RenderBadge(const std::wstring& badge, int pixels, Pixels& out) {
    out.size = pixels;
    out.px.assign(pixels * pixels, 0);
    Bitmap bmp(pixels, pixels, pixels * 4, PixelFormat32bppARGB, (BYTE*)out.px.data());
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.ScaleTransform((REAL)pixels, (REAL)pixels);
    REAL px = 1.0f / pixels;
    bool ok = true;
    int index;
    std::wstring file;
    if (Quoted(badge)) Text(g, badge.substr(1, badge.size() - 2));
    else if (Builtin draw = FindBuiltin(badge)) draw(g, px);
    else if (!(file = BadgeFile(badge, index)).empty()) ok = DrawFile(g, file, index, pixels);
    else Text(g, badge);
    g.Flush(FlushIntentionSync);
    return ok;
}

UINT32 Over(UINT32 dst, UINT32 src) {
    UINT32 sa = src >> 24;
    if (!sa) return dst;
    UINT32 da = dst >> 24;
    if (sa == 255 || !da) return src;
    float s = sa / 255.0f, d = da / 255.0f * (1 - s), a = s + d;
    UINT32 out = (UINT32)(a * 255 + 0.5f) << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        float c = (((src >> shift) & 0xFF) * s + ((dst >> shift) & 0xFF) * d) / a;
        out |= (UINT32)(c + 0.5f) << shift;
    }
    return out;
}

// Dark halo of the given radius under the badge, so it reads on the folder and on any background.
void Composite(Pixels& icon, const Pixels& badge, int x0, int y0, int radius) {
    int n = badge.size, size = icon.size;
    auto alphaAt = [&](int x, int y) -> int {
        return x < 0 || y < 0 || x >= n || y >= n ? 0 : (int)(badge.px[y * n + x] >> 24);
    };
    for (int y = std::max(0, y0 - radius); y < std::min(size, y0 + n + radius); y++)
        for (int x = std::max(0, x0 - radius); x < std::min(size, x0 + n + radius); x++) {
            float halo = 0;
            for (int dy = -radius; dy <= radius; dy++)
                for (int dx = -radius; dx <= radius; dx++) {
                    float w = std::min(1.0f, std::max(0.0f, radius + 0.5f - sqrtf((float)(dx * dx + dy * dy))));
                    halo = std::max(halo, w * alphaAt(x - x0 + dx, y - y0 + dy));
                }
            UINT32& p = icon.px[y * size + x];
            p = Over(p, (UINT32)(halo * kHaloAlpha / 255.0f + 0.5f) << 24);
            if (x >= x0 && y >= y0 && x < x0 + n && y < y0 + n) p = Over(p, badge.px[(y - y0) * n + (x - x0)]);
        }
}

void BadgeBox(int size, int& pos, int& pixels) {
    if (size <= 24) {
        pixels = (int)lround(size * 0.625);
        pos = size - pixels;
    } else {
        pixels = (int)lround(size * 0.56);
        pos = size - pixels - (int)lround(size * 0.03);
    }
}

bool EncoderClsid(const wchar_t* mime, CLSID& clsid) {
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || !bytes) return false;
    std::vector<BYTE> buf(bytes);
    auto codecs = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(count, bytes, codecs) != Ok) return false;
    for (UINT i = 0; i < count; i++)
        if (!wcscmp(codecs[i].MimeType, mime)) {
            clsid = codecs[i].Clsid;
            return true;
        }
    return false;
}

bool PngBytes(const Pixels& p, std::vector<BYTE>& out) {
    CLSID png;
    IStream* stream = nullptr;
    if (!EncoderClsid(L"image/png", png) || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    bool ok;
    {
        Bitmap bmp(p.size, p.size, p.size * 4, PixelFormat32bppARGB, (BYTE*)p.px.data());
        ok = bmp.Save(stream, &png, nullptr) == Ok;
    }
    STATSTG stat;
    LARGE_INTEGER zero = {};
    ULONG read = 0;
    if (ok) ok = SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    if (ok) {
        out.resize((size_t)stat.cbSize.QuadPart);
        ok = SUCCEEDED(stream->Read(out.data(), (ULONG)out.size(), &read)) && read == out.size();
    }
    stream->Release();
    return ok;
}

template <class T> void Put(std::vector<BYTE>& out, T value) {
    out.insert(out.end(), (const BYTE*)&value, (const BYTE*)&value + sizeof(value));
}

void DibBytes(const Pixels& p, std::vector<BYTE>& out) {
    int n = p.size, maskRow = (n + 31) / 32 * 4;
    BITMAPINFOHEADER h = { sizeof(h), n, n * 2, 1, 32, BI_RGB, (DWORD)(n * n * 4 + maskRow * n) };
    Put(out, h);
    for (int y = n - 1; y >= 0; y--)
        out.insert(out.end(), (const BYTE*)&p.px[y * n], (const BYTE*)&p.px[y * n] + n * 4);
    out.insert(out.end(), (size_t)maskRow * n, 0);
}

bool WriteIco(const std::wstring& file, const std::vector<Pixels>& images) {
    std::vector<std::vector<BYTE>> blobs(images.size());
    for (size_t i = 0; i < images.size(); i++) {
        if (images[i].size < 256) DibBytes(images[i], blobs[i]);
        else if (!PngBytes(images[i], blobs[i])) return false;
    }
    std::vector<BYTE> out;
    Put<WORD>(out, 0);
    Put<WORD>(out, 1);
    Put<WORD>(out, (WORD)images.size());
    DWORD offset = (DWORD)(6 + 16 * images.size());
    for (size_t i = 0; i < images.size(); i++) {
        BYTE dim = images[i].size >= 256 ? 0 : (BYTE)images[i].size;
        Put(out, dim);
        Put(out, dim);
        Put<WORD>(out, 0);
        Put<WORD>(out, 1);
        Put<WORD>(out, 32);
        Put<DWORD>(out, (DWORD)blobs[i].size());
        Put<DWORD>(out, offset);
        offset += (DWORD)blobs[i].size();
    }
    for (auto& b : blobs) out.insert(out.end(), b.begin(), b.end());
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr) && written == out.size();
    CloseHandle(f);
    return ok;
}

std::wstring IconDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\ExplorerTint";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\icons";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    CoTaskMemFree(base);
    return dir;
}

// Identifies the drawing; a badge file adds its write time so an edited file gets a new icon.
std::wstring CacheKey(bool repo, const std::wstring& badge) {
    std::wstring key = L"v" + std::to_wstring(kVersion) + (repo ? L"|repo|" : L"|folder|") + badge;
    int index;
    std::wstring file = Quoted(badge) || FindBuiltin(badge) ? L"" : BadgeFile(badge, index);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!file.empty() && GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa))
        key += L"|" + std::to_wstring(((UINT64)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
    return key;
}

int IndexFor(bool repo, const std::wstring& badge) {
    std::wstring key = CacheKey(repo, badge);
    AcquireSRWLockShared(&s_cacheLock);
    auto it = s_cache.find(key);
    int index = it == s_cache.end() ? -2 : it->second;
    ReleaseSRWLockShared(&s_cacheLock);
    if (index != -2) return index;

    UINT64 hash = 14695981039346656037ull;
    for (wchar_t c : key) hash = (hash ^ c) * 1099511628211ull;
    wchar_t name[32];
    swprintf_s(name, L"\\%016llx.ico", hash);
    std::wstring dir = IconDir(), file = dir + name;
    if (!dir.empty() && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring temp = file + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                            std::to_wstring(GetCurrentThreadId()) + L".tmp";
        if (WriteFolderIcon(temp, repo, badge)) MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
        else DeleteFileW(temp.c_str());
    }
    index = !dir.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES
                ? Shell_GetCachedImageIndexW(file.c_str(), 0, 0)
                : -1;
    Log(L"icons: %s -> %s, index %d", key.c_str(), file.c_str(), index);
    AcquireSRWLockExclusive(&s_cacheLock);
    s_cache[key] = index;
    ReleaseSRWLockExclusive(&s_cacheLock);
    return index;
}

}

bool WriteFolderIcon(const std::wstring& file, bool repo, const std::wstring& badge) {
    if (!UseGdiplus()) return false;
    std::vector<Pixels> images;
    for (int size : kSizes) {
        Pixels icon;
        if (!(repo ? RepoBase(size, icon) : FolderBase(size, icon))) return false;
        if (!badge.empty()) {
            int pos, pixels;
            BadgeBox(size, pos, pixels);
            Pixels mark;
            if (RenderBadge(badge, pixels, mark))
                Composite(icon, mark, pos, pos, size < 128 ? 1 : 3);
        }
        images.push_back(std::move(icon));
    }
    return WriteIco(file, images);
}

namespace {

bool InRoots(const Settings& s, const std::wstring& dir) {
    if (s.iconRoots.empty()) return true;
    std::wstring lower = Lower(dir);
    return std::any_of(s.iconRoots.begin(), s.iconRoots.end(),
                       [&](const std::wstring& r) { return lower.compare(0, r.size(), r) == 0; });
}

}

int FolderIconIndex(const Settings& s, const std::wstring& path) {
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= path.size() || (!s.repoIcon && s.folderIcons.empty()) ||
        !InRoots(s, path.substr(0, slash + 1)))
        return -1;
    WIN32_FIND_DATAW fd = {};
    fd.dwFileAttributes = GetFileAttributesW(path.c_str());
    if (fd.dwFileAttributes == INVALID_FILE_ATTRIBUTES || wcscpy_s(fd.cFileName, path.c_str() + slash + 1)) return -1;
    return FolderIconIndex(s, path.substr(0, slash + 1), fd);
}

int FolderIconIndex(const Settings& s, const std::wstring& parent, const WIN32_FIND_DATAW& fd) {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || parent.empty() || (!s.repoIcon && s.folderIcons.empty()))
        return -1;
    std::wstring dir = parent.back() == L'\\' ? parent : parent + L"\\";
    if (!InRoots(s, dir)) return -1;
    std::wstring badge;
    for (auto& p : s.folderIcons)
        if (PathMatchSpecW(fd.cFileName, p.first.c_str())) {
            badge = p.second;
            break;
        }
    bool repo = s.repoIcon && GetFileAttributesW((dir + fd.cFileName + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!repo && badge.empty()) return -1;
    return IndexFor(repo, badge);
}
