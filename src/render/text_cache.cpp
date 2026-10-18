#include "render/text_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "stb_truetype.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace bmj {

namespace {

std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        char32_t v = n == 0 ? c : c & (0x3F >> n);
        for (int k = 1; k <= n && i + k < s.size(); ++k) v = (v << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(v);
        i += n + 1;
    }
    return out;
}

SDL_Texture* make_texture(SDL_Renderer* r, int w, int h, std::vector<uint8_t>& rgba) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, rgba.data(), w * 4);
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_DestroySurface(s);
    if (t) SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

}  // namespace

void TextCache::clear() {
    for (auto& [k, t] : cache_)
        if (t.tex) SDL_DestroyTexture(t.tex);
    cache_.clear();
}

float TextCache::draw(float x, float y, int size, SDL_Color c, const std::string& s, int align, float max_w, int thick) {
    if (s.empty()) return 0;
    Tex t = get(size, thick, s);
    if (!t.tex) return 0;
    float w = static_cast<float>(t.w);
    float sx = (max_w > 0 && w > max_w) ? max_w / w : 1.0f;
    float dw = w * sx;
    if (align == 1) x -= dw / 2;
    else if (align == 2) x -= dw;
    float tw = 0, th = 0;
    SDL_GetTextureSize(t.tex, &tw, &th);
    SDL_SetTextureColorMod(t.tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t.tex, c.a);
    SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    SDL_FRect dst{std::floor(x), std::floor(y), tw * sx, th};
    SDL_RenderTexture(r_, t.tex, nullptr, &dst);
    return dw;
}

const std::vector<uint8_t>* TextCache::font_data(int thick) {
    bool bold = thick >= 6;
    // OSごとの日本語フォントの候補（見つかった最初のもの）。DxLibの太さ（0〜9）はおおまかな太さに当てはめる
    std::vector<std::string> cands;
    if (!font_path_.empty()) cands.push_back(font_path_);
    int w = thick <= 2 ? 3 : thick <= 4 ? 4 : thick <= 6 ? 6 : 8;
    cands.push_back("/System/Library/Fonts/ヒラギノ角ゴシック W" + std::to_string(w) + ".ttc");  // macOS
    cands.push_back(bold ? "C:/Windows/Fonts/YuGothB.ttc" : "C:/Windows/Fonts/YuGothM.ttc");    // Windows
    cands.push_back(bold ? "C:/Windows/Fonts/meiryob.ttc" : "C:/Windows/Fonts/meiryo.ttc");
    cands.push_back("C:/Windows/Fonts/msgothic.ttc");
    cands.push_back(bold ? "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"  // Linux
                         : "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    cands.push_back("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
    for (const std::string& path : cands) {
        auto it = files_.find(path);
        if (it != files_.end()) {
            if (!it->second.empty()) return &it->second;
            continue;
        }
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes;
        if (f) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        auto& slot = files_[path];
        slot = std::move(bytes);
        if (!slot.empty()) return &slot;
    }
    return nullptr;
}

#ifdef _WIN32
// GDI で白い文字を黒い地に描いて、明るさをそのまま不透明度にする
TextCache::Tex TextCache::get_system(int size, const std::string& s) {
    Tex out;
    std::wstring ws = widen(s), face = widen(system_font_);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str());
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE ext{};
    GetTextExtentPoint32W(dc, ws.c_str(), static_cast<int>(ws.size()), &ext);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int w = std::max<int>(1, ext.cx), h = std::max<int>(1, tm.tmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 上から下へ
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        std::fill_n(static_cast<uint32_t*>(bits), static_cast<size_t>(w) * h, 0u);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 0, 0, ws.c_str(), static_cast<int>(ws.size()));
        GdiFlush();
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 255);
        const uint32_t* px = static_cast<const uint32_t*>(bits);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) rgba[i * 4 + 3] = static_cast<uint8_t>((px[i] >> 8) & 0xff);
        out.tex = make_texture(r_, w, h, rgba);
        if (out.tex) SDL_SetTextureScaleMode(out.tex, SDL_SCALEMODE_NEAREST);
        out.w = w;
        out.h = h;
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
}
#else
TextCache::Tex TextCache::get_system(int, const std::string&) { return {}; }
#endif

TextCache::Tex TextCache::get(int size, int thick, const std::string& s) {
    char keybuf[32];
    std::snprintf(keybuf, sizeof(keybuf), "%d/%d/%d/", size, thick, antialias_ ? 1 : 0);
    std::string key = keybuf + s;
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    // 使い捨ての文字列（数字など）でキャッシュが膨らみすぎたら作り直す
    if (cache_.size() > 2000) clear();
#ifdef _WIN32
    if (!system_font_.empty()) {
        Tex t = get_system(size, s);
        cache_[key] = t;
        return t;
    }
#endif
    Tex out;
    const std::vector<uint8_t>* data = font_data(thick);
    if (data && size > 0) {
        stbtt_fontinfo info;
        int face = !font_path_.empty() && data == &files_[font_path_] ? font_face_ : 0;
        int offset = stbtt_GetFontOffsetForIndex(data->data(), face);
        if (offset < 0) offset = stbtt_GetFontOffsetForIndex(data->data(), 0);
        if (stbtt_InitFont(&info, data->data(), offset)) {
            float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(size));
            int ascent, descent, gap;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
            int base = static_cast<int>(ascent * scale);
            std::u32string u = utf32(s);
            float x = 0;
            for (char32_t ch : u) {
                int adv, lsb;
                stbtt_GetCodepointHMetrics(&info, static_cast<int>(ch), &adv, &lsb);
                x += adv * scale;
            }
            int w = std::max(1, static_cast<int>(std::ceil(x)) + 2), h = size;
            std::vector<uint8_t> alpha(static_cast<size_t>(w) * h, 0);
            x = 0;
            for (char32_t ch : u) {
                int adv, lsb, x0, y0, x1, y1;
                stbtt_GetCodepointHMetrics(&info, static_cast<int>(ch), &adv, &lsb);
                stbtt_GetCodepointBitmapBox(&info, static_cast<int>(ch), scale, scale, &x0, &y0, &x1, &y1);
                int gx = static_cast<int>(x) + x0, gy = base + y0;
                int gw = x1 - x0, gh = y1 - y0;
                if (gw > 0 && gh > 0) {
                    std::vector<uint8_t> g(static_cast<size_t>(gw) * gh);
                    stbtt_MakeCodepointBitmap(&info, g.data(), gw, gh, gw, scale, scale, static_cast<int>(ch));
                    for (int yy = 0; yy < gh; ++yy)
                        for (int xx = 0; xx < gw; ++xx) {
                            int px = gx + xx, py = gy + yy;
                            if (px < 0 || py < 0 || px >= w || py >= h) continue;
                            uint8_t& dstp = alpha[static_cast<size_t>(py) * w + px];
                            dstp = std::max(dstp, g[static_cast<size_t>(yy) * gw + xx]);
                        }
                }
                x += adv * scale;
            }
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 255);
            for (size_t i = 0; i < alpha.size(); ++i) rgba[i * 4 + 3] = antialias_ ? alpha[i] : (alpha[i] >= 128 ? 255 : 0);
            out.tex = make_texture(r_, w, h, rgba);
            if (out.tex && !antialias_) SDL_SetTextureScaleMode(out.tex, SDL_SCALEMODE_NEAREST);
            out.w = static_cast<int>(std::ceil(x));
            out.h = h;
        }
    }
    cache_[key] = out;
    return out;
}

}  // namespace bmj
