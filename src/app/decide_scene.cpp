#include "app/decide_scene.h"

#include <filesystem>

#include "audio/system_sound.h"
#include "input/input.h"
#include "render/frame_hooks.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// 譜面の STAGEFILE のパス（拡張子だけ違うことがよくある）
std::string stagefile_path(const ChartRecord& c) {
    if (c.stagefile.empty()) return {};
    fs::path p = fs::path(c.folder) / c.stagefile;
    std::error_code ec;
    if (fs::exists(p, ec)) return p.string();
    for (const char* ext : {".png", ".jpg", ".bmp", ".PNG", ".JPG", ".BMP"}) {
        fs::path q = p;
        q.replace_extension(ext);
        if (fs::exists(q, ec)) return q.string();
    }
    return {};
}

}  // namespace

// 流れ（LR2 の決定画面）：
//   #SCENETIME が経ったらプレイへ。#STARTINPUT（と #IGNOREINPUT）の後に鍵盤・Enter・左クリックでタイマー3 を立て、#SKIP の後にプレイへ。
//   ESC・右クリック・START+SELECT でタイマー2 を立て、#FADEOUT の後に選曲へ戻る
DecideResult run_decide(App& app, const SelectContext& base) {
    SelectContext ctx = base;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = ctx.option(i);
    app.load_decide_skin(ops);
    Lr2Skin* sk = app.decide_skin.get();
    if (!sk) return DecideResult::Play;
    ctx.skin = sk;
    app.set_view(sk->width, sk->height);

    // STAGEFILE（特別な画像 100）
    SDL_Texture* stage = nullptr;
    if (ctx.chart) {
        std::string p = stagefile_path(*ctx.chart);
        SkinImage img;
        static const uint8_t black[3] = {0, 0, 0};
        if (!p.empty() && load_skin_image(p, img, black, false)) stage = app.decide_renderer->create_texture(img);
    }
    app.decide_renderer->set_image_override(100, stage);
    ctx.has_stagefile = stage != nullptr;

    app.mixer.play_system(kSysDecide);
    InputMapper input(app.keycfg);
    input.set_min_hold_ms(app.cfg.input_interval);
    SkinTimers timers;
    timers.set(0, 0);
    timers.set(11, 0);
    const uint64_t start = SDL_GetTicksNS();
    auto now_ms = [&] { return (SDL_GetTicksNS() - start) / 1e6; };
    DecideResult result = DecideResult::Play;
    bool done = false;
    while (!done) {
        double t = now_ms();
        bool accept = t > sk->startinput && t > sk->ignore_input;
        auto skip = [&] {
            if (accept && !timers.on(3) && !timers.on(2)) timers.set(3, t);
        };
        auto cancel = [&] {
            if (!timers.on(2)) timers.set(2, t);
        };
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) result = DecideResult::Quit, done = true;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_ESCAPE) cancel();
                else if (e.key.key == SDLK_RETURN) skip();
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_LEFT) skip();
                else if (e.button.button == SDL_BUTTON_RIGHT) cancel();
            }
            input.handle(e);
        }
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        input.collect(SDL_GetTicksNS(), lanes, buttons);
        for (const ButtonInput& b : buttons)
            if (b.down && b.button >= kKey1 && b.button <= kKey7) skip();
        if (input.button_held(kStart) && input.button_held(kSelect)) cancel();
        if (!timers.on(1) && t > sk->startinput) timers.set(1, t);

        if (timers.on(2) && timers.lapse(2, t) >= sk->fadeout) result = DecideResult::Back, done = true;
        else if (timers.on(3) && timers.lapse(3, t) >= sk->close) done = true;
        else if (!timers.on(2) && sk->scenetime > 0 && t >= sk->scenetime) done = true;

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
        app.decide_renderer->draw_select(*sk, st);
        if (const char* shot = std::getenv("BMJ_DECIDE_SHOT"); shot && t > 1500) {
            // 開発用：1 枚保存して戻る
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            result = DecideResult::Back, done = true;
        }
        frame_hooks_before_present(app.renderer);
        SDL_RenderPresent(app.renderer);
        SDL_DelayPrecise(2'000'000);
    }
    app.decide_renderer->set_image_override(100, nullptr);
    if (stage) SDL_DestroyTexture(stage);
    return result;
}

}  // namespace bmj
