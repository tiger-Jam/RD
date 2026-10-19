#include "render/frame_hooks.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace bmj {

namespace {

struct State {
    SDL_Texture* frame = nullptr;  // スキンの解像度の描き先
    bool installed = false;
    bool shot = false, show_fps = false;
    std::function<void()> on_shot;
    uint64_t second_start = 0;
    int frames = 0, fps = 0;
} g;

bool SDLCALL watch(void*, SDL_Event* e) {
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat) return true;
    if (e->key.key == SDLK_F6) g.shot = true;
    else if (e->key.key == SDLK_F7) g.show_fps = !g.show_fps;
    return true;
}

// 保存先：実行ファイルのあるフォルダの screenshot（LR2 と同じく、ゲームのフォルダの下）
fs::path shot_path() {
    const char* base = SDL_GetBasePath();
    fs::path dir = fs::path(base ? base : "") / "screenshot";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "BMjoy %04d-%02d-%02d %02d-%02d-%02d.png", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return dir / name;
}

// 描いた画面を、スキンの解像度（論理解像度）の画像にして保存する
void save_shot(SDL_Renderer* r) {
    SDL_Surface* full = SDL_RenderReadPixels(r, nullptr);
    if (!full) return;
    int lw = 0, lh = 0;
    SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(r, &lw, &lh, &mode);
    SDL_FRect area{};
    SDL_Surface* out = full;
    if (lw > 0 && lh > 0 && SDL_GetRenderLogicalPresentationRect(r, &area) && (full->w != lw || full->h != lh)) {
        // 枠（上下か左右の黒帯）を除いて、スキンの大きさに縮める
        SDL_Rect src{static_cast<int>(area.x), static_cast<int>(area.y), static_cast<int>(area.w), static_cast<int>(area.h)};
        if (SDL_Surface* scaled = SDL_CreateSurface(lw, lh, full->format)) {
            SDL_BlitSurfaceScaled(full, &src, scaled, nullptr, SDL_SCALEMODE_LINEAR);
            out = scaled;
        }
    }
    fs::path p = shot_path();
    std::u8string u8 = p.u8string();
    if (SDL_SavePNG(out, reinterpret_cast<const char*>(u8.c_str())) && g.on_shot) g.on_shot();
    if (out != full) SDL_DestroySurface(out);
    SDL_DestroySurface(full);
}

}  // namespace

void frame_hooks_install(std::function<void()> on_shot) {
    g.on_shot = std::move(on_shot);
    if (!g.installed) SDL_AddEventWatch(watch, nullptr), g.installed = true;
}

void frame_hooks_before_present(SDL_Renderer* r) {
    uint64_t now = SDL_GetTicks();
    g.frames++;
    if (now - g.second_start >= 1000) g.fps = g.frames, g.frames = 0, g.second_start = now;
    if (g.show_fps) {
        char s[32];
        std::snprintf(s, sizeof(s), "FPS %d", g.fps);
        Uint8 cr, cg, cb, ca;
        SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderDebugText(r, 3, 3, s);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, 2, 2, s);
        SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    }
    if (g.shot) {
        g.shot = false;
        save_shot(r);
    }
}

namespace {

// 描き先の大きさ：スキンの解像度の整数倍のうち、ウィンドウに収まる一番大きいもの（半端な分だけを最後になめらかに拡大する。
// 整数倍のところは絵ごとの指定（なめらかにするか）のまま描くので、高 DPI や 4K でもぼやけない）
int frame_scale(SDL_Renderer* r, int w, int h) {
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(r, &ow, &oh);
    return std::max(1, std::min(ow / w, oh / h));
}

bool make_frame(SDL_Renderer* r, int w, int h) {
    const int k = frame_scale(r, w, h);
    if (g.frame && g.frame->w == w * k && g.frame->h == h * k) return true;
    if (g.frame) SDL_DestroyTexture(g.frame), g.frame = nullptr;
    g.frame = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w * k, h * k);
    if (!g.frame) return false;
    SDL_SetTextureScaleMode(g.frame, SDL_SCALEMODE_LINEAR);
    SDL_SetRenderTarget(r, g.frame);
    SDL_SetRenderLogicalPresentation(r, w, h, SDL_LOGICAL_PRESENTATION_STRETCH);  // 描き先の中ではスキンの座標
    SDL_SetRenderTarget(r, nullptr);
    return true;
}

int g_w = 0, g_h = 0;  // スキンの解像度（0 ならウィンドウに直接描いている）

}  // namespace

void frame_target_set(SDL_Renderer* r, int w, int h) {
    SDL_SetRenderTarget(r, nullptr);
    g_w = g_h = 0;
    if (w <= 0 || h <= 0) return;
    // ウィンドウの側：描き先の絵を枠付きで拡大する（マウスの座標もこれでスキンの座標に直る）
    SDL_SetRenderLogicalPresentation(r, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (!make_frame(r, w, h)) return;  // 描き先を作れないときは、前と同じくウィンドウに直接描く
    g_w = w, g_h = h;
    SDL_SetRenderTarget(r, g.frame);
}

void frame_present(SDL_Renderer* r) {
    frame_hooks_before_present(r);
    if (g.frame && g_w > 0 && SDL_GetRenderTarget(r) == g.frame) {
        Uint8 cr, cg, cb, ca;
        SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
        SDL_SetRenderTarget(r, nullptr);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_RenderTexture(r, g.frame, nullptr, nullptr);
        SDL_RenderPresent(r);
        make_frame(r, g_w, g_h);  // ウィンドウの大きさが変わっていたら作り直す
        SDL_SetRenderTarget(r, g.frame);
        SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
        return;
    }
    SDL_RenderPresent(r);
}

}  // namespace bmj
