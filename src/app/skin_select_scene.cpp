#include "app/skin_select_scene.h"

#include <algorithm>
#include <cmath>

#include "app/select_context.h"
#include "audio/system_sound.h"

namespace bmj {

bool run_skin_select(App& app, SkinCatalog& catalog) {
    SelectContext ctx;
    ctx.cfg = &app.cfg;
    ctx.catalog = &catalog;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = ctx.option(i);
    SkinRenderer renderer(app.renderer);
    std::string path = resolve_lr2_path(app.lr2_root, "LR2files/Theme/LR2/SkinSelect/skinselect.lr2skin", false);
    SkinHeader header;
    Lr2Skin skin;
    std::string err;
    if (path.empty() || !read_skin_header(app.lr2_root, path, header) || !load_lr2_skin(app.lr2_root, path, header, ops, skin, err)) {
        std::printf("スキンセレクトのスキンを読めない: %s\n", err.c_str());
        return false;
    }
    if (!header.has_resolution) skin.width = 640, skin.height = 480;
    renderer.upload(skin);
    ctx.skin = &skin;
    app.set_view(skin.width, skin.height);

    SkinTimers timers;
    timers.set(0, 0);
    const uint64_t start = SDL_GetTicksNS();
    auto now_ms = [&] { return (SDL_GetTicksNS() - start) / 1e6; };
    bool select_changed = false, done = false;
    const SkinObject* dragging = nullptr;
    float mouse_x = -1, mouse_y = -1;
    while (!done) {
        double t = now_ms();
        if (!timers.on(1) && t > skin.startinput) timers.set(1, t);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) timers.set(2, t);
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                SDL_ConvertEventToRenderCoordinates(app.renderer, &e);
            if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y;
            // ESC・右クリックで閉じる（#FADEOUT のあと選曲へ）
            if ((e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) ||
                (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT))
                if (!timers.on(2)) timers.set(2, t);
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) dragging = nullptr;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && !timers.on(2)) {
                float mx = e.button.x, my = e.button.y;
                for (const SkinObject& o : skin.buttons) {
                    if (o.src.op[1] != 1 || o.dst.empty() || !timers.on(o.dst.timer)) continue;
                    SkinDraw d = eval_dst(o.dst, timers.lapse(o.dst.timer, t));
                    if (!d.visible) continue;
                    float x0 = std::min(d.x, d.x + d.w), x1 = std::max(d.x, d.x + d.w);
                    float y0 = std::min(d.y, d.y + d.h), y1 = std::max(d.y, d.y + d.h);
                    if (mx < x0 || mx >= x1 || my < y0 || my >= y1) continue;
                    int dir = o.src.op[3] == 1 ? 1 : o.src.op[3] == 2 ? -1 : (mx >= (x0 + x1) / 2 ? 1 : -1);
                    int ty = o.src.op[0];
                    if (ty >= 170 && ty <= 188) {
                        catalog.select_type(ty - 170, app.cfg);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 190 || (ty >= 220 && ty <= 229)) {
                        bool changed = ty == 190 ? catalog.step_skin(app.cfg, dir) : catalog.step_custom(app.cfg, ty - 220, dir);
                        if (changed) {
                            app.mixer.play_system(kSysOptionChange);
                            if (catalog.selected_type() == 5) select_changed = true;
                        }
                    }
                    break;
                }
                for (const SkinObject& o : skin.sliders) {
                    if (o.src.op[2] != 7 || o.dst.empty() || !timers.on(o.dst.timer)) continue;
                    SkinDraw d = eval_dst(o.dst, timers.lapse(o.dst.timer, t));
                    float len = static_cast<float>(o.src.op[1]);
                    SDL_FRect rc{d.x, d.y, d.w, d.h};
                    if (o.src.op[0] == 0) rc.y -= len, rc.h += len;
                    else if (o.src.op[0] == 1) rc.w += len;
                    else if (o.src.op[0] == 2) rc.h += len;
                    else rc.x -= len, rc.w += len;
                    if (mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h) dragging = &o;
                }
            }
            if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                // カスタマイズ項目のスクロール（スライダー7）
                const SkinSrc& src = dragging->src;
                SkinDraw d = eval_dst(dragging->dst, timers.lapse(dragging->dst.timer, t));
                float len = static_cast<float>(std::max(1, src.op[1]));
                float r = src.op[0] == 0 ? (d.y + d.h / 2 - mouse_y) / len : src.op[0] == 1 ? (mouse_x - d.x - d.w / 2) / len
                          : src.op[0] == 2 ? (mouse_y - d.y - d.h / 2) / len : (d.x + d.w / 2 - mouse_x) / len;
                catalog.set_scroll(static_cast<int>(std::lround(std::clamp(r, 0.0f, 1.0f) * catalog.scroll_max(app.cfg))), app.cfg);
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) catalog.set_scroll(catalog.scroll() - static_cast<int>(e.wheel.y), app.cfg);
        }
        if (timers.on(2) && timers.lapse(2, t) >= skin.fadeout) done = true;

        PlaySkinState st;
        st.now_ms = t;
        st.timers = &timers;
        st.option = [&](int o) { return ctx.option(o); };
        st.number = [&](int id) { return ctx.number(id); };
        st.bargraph = [&](int ty, double& v, double& m) { return ctx.bargraph(ty, v, m); };
        st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx.slider(ty, v, lo, hi); };
        st.text = [&](int id) { return ctx.text(id); };
        st.button = [&](int ty) { return ctx.button(ty); };
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        renderer.draw_select(skin, st);
        if (const char* shot = std::getenv("BMJ_SKINSELECT_SHOT"); shot && t > 800) {
            // 開発用：1 枚保存して閉じる
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            done = true;
        }
        SDL_RenderPresent(app.renderer);
        SDL_DelayPrecise(2'000'000);
    }
    return select_changed;
}

}  // namespace bmj
