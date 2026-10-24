#include "VectorImage.h"
#include <algorithm>
#include <cctype>
#include <d2d1_3.h>
#include <map>
#include <regex>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

const int kMaxDepth = 4;            // nested drawables
const float kAdaptiveBleed = 0.25f;  // an adaptive icon shows the inner 72 of its 108 units: 18 / 72 on each side
const float kAdaptiveCorner = 0.22f;

// Explorer's threads have COM already; IconTest's do not.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

ID2D1Factory1* D2d() {
    static ID2D1Factory1* factory = [] {
        ID2D1Factory1* f = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&f);
        return f;
    }();
    return factory;
}

std::string ReadFileText(const std::wstring& path) {
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    char buf[65536];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got && text.size() < 4 * 1024 * 1024) text.append(buf, got);
    CloseHandle(f);
    return text;
}

std::wstring Ext(const std::wstring& file) {
    return Lower(PathFindExtensionW(file.c_str()));
}

bool Decode(IWICImagingFactory* wic, const std::wstring& file, REFWICPixelFormatGUID format, ComPtr<IWICBitmapSource>& out) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    return SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
           SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
           SUCCEEDED(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(converter.As(&out));
}

// A start or end tag of an XML document; attribute names lose their namespace prefix ("android:src" -> "src").
struct Tag {
    std::string name;
    bool closing = false;
    std::map<std::string, std::string> attrs;

    std::string Get(const char* key) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? "" : it->second;
    }
    float Number(const char* key, float fallback) const {
        std::string v = Get(key);
        return v.empty() ? fallback : (float)atof(v.c_str());
    }
};

std::string Entities(std::string s) {
    static const std::pair<const char*, const char*> table[] = { { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&amp;", "&" } };
    for (auto& [from, to] : table)
        for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += strlen(to)) s.replace(p, strlen(from), to);
    return s;
}

std::vector<Tag> Tags(const std::string& xml) {
    std::vector<Tag> tags;
    for (size_t pos = xml.find('<'); pos != std::string::npos; pos = xml.find('<', pos)) {
        if (xml.compare(pos, 4, "<!--") == 0) {
            size_t end = xml.find("-->", pos);
            if (end == std::string::npos) break;
            pos = end + 3;
            continue;
        }
        size_t end = pos + 1;
        for (char quote = 0; end < xml.size(); end++) {
            char c = xml[end];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (end >= xml.size()) break;
        std::string body = xml.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (body.empty() || body[0] == '?' || body[0] == '!') continue;
        Tag tag;
        if (body[0] == '/') {
            tag.closing = true;
            body.erase(0, 1);
        }
        if (!body.empty() && body.back() == '/') body.pop_back();
        size_t i = 0;
        while (i < body.size() && !isspace((unsigned char)body[i])) i++;
        tag.name = body.substr(0, i);
        while (i < body.size()) {
            size_t eq = body.find('=', i), quote = eq == std::string::npos ? eq : body.find_first_of("\"'", eq);
            if (quote == std::string::npos) break;
            size_t close = body.find(body[quote], quote + 1);
            if (close == std::string::npos) break;
            std::string key = body.substr(i, eq - i);
            key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return isspace((unsigned char)c); }), key.end());
            size_t colon = key.find(':');
            if (colon != std::string::npos) key = key.substr(colon + 1);
            tag.attrs[key] = Entities(body.substr(quote + 1, close - quote - 1));
            i = close + 1;
        }
        tags.push_back(std::move(tag));
    }
    return tags;
}

// The resource folder of an Android drawable (...\res\drawable\x.xml -> ...\res\) and its named colours.
struct Resources {
    std::wstring dir;
    std::map<std::string, std::string> colors;

    explicit Resources(const std::wstring& file) {
        std::wstring d = file.substr(0, file.find_last_of(L'\\'));
        dir = d.substr(0, d.find_last_of(L'\\') + 1);
        static const std::regex color(R"re(<color\s+name\s*=\s*"([^"]+)"\s*>\s*([^<\s]+)\s*</color>)re");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"values*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring values = dir + fd.cFileName + L"\\";
            WIN32_FIND_DATAW xml;
            HANDLE x = FindFirstFileW((values + L"*.xml").c_str(), &xml);
            if (x == INVALID_HANDLE_VALUE) continue;
            do {
                std::string text = ReadFileText(values + xml.cFileName);
                for (std::sregex_iterator it(text.begin(), text.end(), color), end; it != end; ++it)
                    colors.emplace((*it)[1], (*it)[2]);
            } while (FindNextFileW(x, &xml));
            FindClose(x);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // "@drawable/name" or "@mipmap/name": an adaptive icon first, then the densest bitmap, then any drawable.
    std::wstring Find(const std::string& ref) const {
        size_t slash = ref.find('/');
        if (ref.empty() || ref[0] != '@' || slash == std::string::npos) return L"";
        std::wstring name(ref.begin() + slash + 1, ref.end());
        static const wchar_t* const densities[] = { L"xxxhdpi", L"xxhdpi", L"xhdpi", L"hdpi", L"mdpi" };
        std::wstring best;
        int bestRank = INT_MAX;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return L"";
        do {
            std::wstring sub = Lower(fd.cFileName);
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (sub.compare(0, 6, L"mipmap") && sub.compare(0, 8, L"drawable"))) continue;
            for (const wchar_t* ext : { L".xml", L".png", L".webp" }) {
                std::wstring file = dir + fd.cFileName + L"\\" + name + ext;
                if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                int rank = 10;
                if (!wcscmp(ext, L".xml")) rank = sub.find(L"anydpi") != std::wstring::npos ? 0 : 20;
                else
                    for (int i = 0; i < (int)ARRAYSIZE(densities); i++)
                        if (sub.find(densities[i]) != std::wstring::npos) {
                            rank = 1 + i;
                            break;
                        }
                if (rank < bestRank) bestRank = rank, best = file;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return best;
    }

    // #RGB, #ARGB, #RRGGBB, #AARRGGBB or a reference to one.
    bool Color(std::string value, UINT32& argb, int depth = 0) const {
        if (value == "@android:color/transparent") value = "#00000000";
        else if (value == "@android:color/white") value = "#FFFFFFFF";
        else if (value == "@android:color/black") value = "#FF000000";
        if (value.compare(0, 7, "@color/") == 0) {
            auto it = colors.find(value.substr(7));
            return it != colors.end() && depth < kMaxDepth && Color(it->second, argb, depth + 1);
        }
        if (value.empty() || value[0] != '#' || value.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) return false;
        std::string hex = value.substr(1);
        if (hex.size() == 3 || hex.size() == 4) {
            std::string wide;
            for (char c : hex) wide += std::string(2, c);
            hex = wide;
        }
        if (hex.size() == 6) hex = "FF" + hex;
        if (hex.size() != 8) return false;
        argb = (UINT32)strtoul(hex.c_str(), nullptr, 16);
        return true;
    }
};

std::string Num(float v) {
    char buf[32];
    sprintf_s(buf, "%g", v);
    return buf;
}

std::string SvgColor(UINT32 argb) {
    char buf[8];
    sprintf_s(buf, "#%06X", argb & 0xFFFFFF);
    return buf;
}

std::string Attr(const std::string& v) {
    std::string out;
    for (char c : v) out += c == '\'' ? "&apos;" : c == '<' ? "&lt;" : c == '&' ? "&amp;" : std::string(1, c);
    return out;
}

// An Android vector drawable as SVG: groups become transformed <g>, paths keep their fill and stroke.
std::string VectorToSvg(const std::vector<Tag>& tags, const Resources& res) {
    std::string svg;
    for (auto& t : tags) {
        if (t.name == "vector" && !t.closing) {
            float w = t.Number("viewportWidth", t.Number("width", 24)), h = t.Number("viewportHeight", t.Number("height", 24));
            svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + Num(w) + " " + Num(h) + "'>";
        } else if (t.name == "group") {
            if (t.closing) {
                svg += "</g>";
                continue;
            }
            float px = t.Number("pivotX", 0), py = t.Number("pivotY", 0);
            svg += "<g transform='translate(" + Num(t.Number("translateX", 0) + px) + " " + Num(t.Number("translateY", 0) + py) +
                   ") rotate(" + Num(t.Number("rotation", 0)) + ") scale(" + Num(t.Number("scaleX", 1)) + " " +
                   Num(t.Number("scaleY", 1)) + ") translate(" + Num(-px) + " " + Num(-py) + ")'>";
        } else if (t.name == "path" && !t.closing) {
            std::string d = t.Get("pathData");
            if (d.empty()) continue;
            svg += "<path d='" + Attr(d) + "'";
            UINT32 c;
            if (res.Color(t.Get("fillColor"), c) && (c >> 24))
                svg += " fill='" + SvgColor(c) + "' fill-opacity='" + Num((c >> 24) / 255.0f * t.Number("fillAlpha", 1)) + "'";
            else
                svg += " fill='none'";
            if (t.Get("fillType") == "evenOdd") svg += " fill-rule='evenodd'";
            float width = t.Number("strokeWidth", 0);
            if (width > 0 && res.Color(t.Get("strokeColor"), c) && (c >> 24)) {
                svg += " stroke='" + SvgColor(c) + "' stroke-opacity='" + Num((c >> 24) / 255.0f * t.Number("strokeAlpha", 1)) +
                       "' stroke-width='" + Num(width) + "' stroke-miterlimit='" + Num(t.Number("strokeMiterLimit", 4)) + "'";
                std::string cap = t.Get("strokeLineCap"), join = t.Get("strokeLineJoin");
                if (!cap.empty()) svg += " stroke-linecap='" + cap + "'";
                if (!join.empty()) svg += " stroke-linejoin='" + join + "'";
            }
            svg += "/>";
        }
    }
    return svg.empty() ? svg : svg + "</svg>";
}

class Painter {
public:
    Painter(IWICImagingFactory* wic, ID2D1DeviceContext5* dc) : m_wic(wic), m_dc(dc) {}

    bool Svg(const std::string& text, const D2D1_RECT_F& rect) {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream((const BYTE*)text.data(), (UINT)text.size()));
        ComPtr<ID2D1SvgDocument> doc;
        ComPtr<ID2D1SvgElement> root;
        if (!stream || FAILED(m_dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(rect.right - rect.left, rect.bottom - rect.top), &doc)))
            return false;
        doc->GetRoot(&root);
        if (!root) return false;
        // Fit the drawing to the square: a viewBox from width and height when it has none, then 100 % of the viewport.
        if (!root->IsAttributeSpecified(L"viewBox", nullptr)) {
            D2D1_SVG_LENGTH w, h;
            if (SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &w, sizeof(w))) &&
                SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &h, sizeof(h))) &&
                w.units == D2D1_SVG_LENGTH_UNITS_NUMBER && h.units == D2D1_SVG_LENGTH_UNITS_NUMBER && w.value > 0 && h.value > 0) {
                D2D1_SVG_VIEWBOX box = { 0, 0, w.value, h.value };
                root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box, sizeof(box));
            }
        }
        D2D1_SVG_LENGTH full = { 100, D2D1_SVG_LENGTH_UNITS_PERCENTAGE };
        root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &full, sizeof(full));
        root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &full, sizeof(full));
        D2D1_MATRIX_3X2_F saved;
        m_dc->GetTransform(&saved);
        m_dc->SetTransform(D2D1::Matrix3x2F::Translation(rect.left, rect.top) * saved);
        m_dc->DrawSvgDocument(doc.Get());
        m_dc->SetTransform(saved);
        return true;
    }

    bool Bitmap(const std::wstring& file, const D2D1_RECT_F& rect) {
        ComPtr<IWICBitmapSource> source;
        ComPtr<ID2D1Bitmap> bitmap;
        if (!Decode(m_wic, file, GUID_WICPixelFormat32bppPBGRA, source) || FAILED(m_dc->CreateBitmapFromWicBitmap(source.Get(), nullptr, &bitmap)))
            return false;
        m_dc->DrawBitmap(bitmap.Get(), rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        return true;
    }

    bool Fill(UINT32 argb, const D2D1_RECT_F& rect) {
        ComPtr<ID2D1SolidColorBrush> brush;
        D2D1_COLOR_F c = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f };
        if (FAILED(m_dc->CreateSolidColorBrush(c, &brush))) return false;
        m_dc->FillRectangle(rect, brush.Get());
        return true;
    }

    // A colour or a drawable reference.
    bool Ref(const std::string& ref, const Resources& res, const D2D1_RECT_F& rect, int depth) {
        UINT32 c;
        if (res.Color(ref, c)) return Fill(c, rect);
        std::wstring file = res.Find(ref);
        return !file.empty() && File(file, rect, depth + 1);
    }

    bool File(const std::wstring& file, const D2D1_RECT_F& rect, int depth = 0) {
        if (depth > kMaxDepth) return false;
        std::wstring ext = Ext(file);
        if (ext == L".svg") return Svg(ReadFileText(file), rect);
        if (ext != L".xml") return Bitmap(file, rect);
        std::vector<Tag> tags = Tags(ReadFileText(file));
        auto root = std::find_if(tags.begin(), tags.end(), [](const Tag& t) { return !t.closing; });
        if (root == tags.end()) return false;
        Resources res(file);
        if (root->name == "vector") return Svg(VectorToSvg(tags, res), rect);
        if (root->name == "adaptive-icon") return Adaptive(tags, res, rect, depth);
        if (root->name == "shape") {
            auto solid = std::find_if(tags.begin(), tags.end(), [](const Tag& t) { return t.name == "solid" && !t.closing; });
            UINT32 c;
            return solid != tags.end() && res.Color(solid->Get("color"), c) && Fill(c, rect);
        }
        if (root->name == "bitmap") return Ref(root->Get("src"), res, rect, depth);
        if (root->name == "inset") return Ref(root->Get("drawable"), res, rect, depth);
        if (root->name == "layer-list") {
            bool any = false;
            for (auto& t : tags)
                if (t.name == "item" && !t.closing && !t.Get("drawable").empty()) any |= Ref(t.Get("drawable"), res, rect, depth);
            return any;
        }
        return false;
    }

private:
    IWICImagingFactory* m_wic;
    ID2D1DeviceContext5* m_dc;

    // Background and foreground layers of 108 units, of which the inner 72 show, in a rounded square.
    bool Adaptive(const std::vector<Tag>& tags, const Resources& res, const D2D1_RECT_F& rect, int depth) {
        float w = rect.right - rect.left, bleed = w * kAdaptiveBleed, r = w * kAdaptiveCorner;
        D2D1_RECT_F layer = { rect.left - bleed, rect.top - bleed, rect.right + bleed, rect.bottom + bleed };
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        if (FAILED(D2d()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, r, r), &shape))) return false;
        m_dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
        bool any = false;
        for (const char* name : { "background", "foreground" }) {
            auto t = std::find_if(tags.begin(), tags.end(), [&](const Tag& x) { return x.name == name && !x.closing; });
            if (t != tags.end() && !t->Get("drawable").empty()) any |= Ref(t->Get("drawable"), res, layer, depth);
        }
        m_dc->PopLayer();
        return any;
    }
};

}

bool LoadRaster(const std::wstring& file, int& width, int& height, std::vector<UINT32>& argb) {
    ComScope com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapSource> source;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        !Decode(wic.Get(), file, GUID_WICPixelFormat32bppBGRA, source) || FAILED(source->GetSize(&w, &h)) || !w || !h ||
        w > 4096 || h > 4096)
        return false;
    argb.assign((size_t)w * h, 0);
    if (FAILED(source->CopyPixels(nullptr, w * 4, (UINT)argb.size() * 4, (BYTE*)argb.data()))) return false;
    width = (int)w;
    height = (int)h;
    return true;
}

bool IsVector(const std::wstring& file) {
    std::wstring ext = Ext(file);
    return ext == L".svg" || ext == L".xml";
}

bool RenderVector(const std::wstring& file, int size, std::vector<UINT32>& argb) {
    ComScope com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext5> dc;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (size <= 0 || !D2d() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bitmap)) ||
        FAILED(D2d()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc)))
        return false;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    bool drawn = Painter(wic.Get(), dc.Get()).File(file, D2D1::RectF(0, 0, (float)size, (float)size));
    if (FAILED(dc->EndDraw()) || !drawn) return false;
    argb.assign((size_t)size * size, 0);
    WICRect rc = { 0, 0, size, size };
    if (FAILED(bitmap->CopyPixels(&rc, size * 4, (UINT)argb.size() * 4, (BYTE*)argb.data()))) return false;
    for (auto& p : argb) {
        UINT32 a = p >> 24;
        if (!a || a == 255) continue;
        UINT32 out = a << 24;
        for (int shift = 0; shift < 24; shift += 8) out |= std::min<UINT32>(255, (((p >> shift) & 0xFF) * 255 + a / 2) / a) << shift;
        p = out;
    }
    return true;
}
