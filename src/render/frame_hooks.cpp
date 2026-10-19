#include "render/frame_hooks.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace bmj {

namespace {

struct State {
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

}  // namespace bmj
