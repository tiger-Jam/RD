#include "app/decide_scene.h"

#include "audio/system_sound.h"
#include "input/input.h"
#include "render/frame_hooks.h"
#include "util/path.h"
#include "util/dev.h"

namespace bmj {

namespace {

// 譜面に書かれた画像（STAGEFILE / BACKBMP / BANNER）を読んでテクスチャにする。無い・読めないときは nullptr。
// 探し方は BGA と同じ（拡張子を替える、1 つ上のフォルダも見る）。抜き色は使わない
SDL_Texture* load_chart_image(App& app, const DirIndex& dir, const std::string& name) {
    std::string p = dir.find_image(name);
    if (p.empty()) return nullptr;
    SkinImage img;
    static const uint8_t black[3] = {0, 0, 0};
    if (!load_skin_image(p, img, black, false)) return nullptr;
    return app.decide_renderer->create_texture(img);
}

}  // namespace

// 流れ（LR2 の決定画面）：
//   何も押さずに #SCENETIME を過ぎたらプレイへ（#SCENETIME の無いスキンは 0 なので、すぐプレイへ）。
//   #STARTINPUT（と #IGNOREINPUT）の後に鍵盤・Enter・左クリックでタイマー3 を立て、#CLOSE の後にプレイへ。
//   #STARTINPUT の後に ESC・右クリック・START+SELECT でタイマー2 を立て、システム音を止め、#FADEOUT の後に選曲へ戻る。
//   飛ばすか戻るかを一度決めたら、もう片方は受けない（#SCENETIME でも進まない）
DecideResult run_decide(App& app, const SelectContext& base) {
    SelectContext ctx = base;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = ctx.option(i);
    app.load_decide_skin(ops);
    Lr2Skin* sk = app.decide_skin.get();
    if (!sk) return DecideResult::Play;
    ctx.skin = sk;
    app.set_view(sk->width, sk->height);

    // 特別な画像 100 STAGEFILE / 101 BACKBMP / 102 BANNER。読めたかどうかが op 190..195 になる
    SDL_Texture* stage = nullptr;
    SDL_Texture* back = nullptr;
    SDL_Texture* banner = nullptr;
    if (ctx.chart) {
        DirIndex dir(ctx.chart->folder);
        stage = load_chart_image(app, dir, ctx.chart->stagefile);
        back = load_chart_image(app, dir, ctx.chart->backbmp);
        banner = load_chart_image(app, dir, ctx.chart->banner);
    }
    app.decide_renderer->set_image_override(100, stage);
    app.decide_renderer->set_image_override(101, back);
    app.decide_renderer->set_image_override(102, banner);
    ctx.has_stagefile = stage != nullptr;
    ctx.has_banner = banner != nullptr;
    const bool has_back = back != nullptr;

    app.mixer.play_system(kSysDecide);
    InputMapper input(app.keycfg);
    input.set_min_hold_ms(app.cfg.input_interval);
    SkinTimers timers;
    timers.set(0, 0);
    timers.set(11, 0);
    const uint64_t start = SDL_GetTicksNS();
    auto now_ms = [&] { return (SDL_GetTicksNS() - start) / 1e6; };
    enum class Phase { Wait, Back, Skip } phase = Phase::Wait;
    DecideResult result = DecideResult::Play;
    bool done = false;
    while (!done) {
        double t = now_ms();
        bool want_skip = false, want_back = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) result = DecideResult::Quit, done = true;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_ESCAPE) want_back = true;
                else if (e.key.key == SDLK_RETURN) want_skip = true;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (e.button.button == SDL_BUTTON_LEFT) want_skip = true;
                else if (e.button.button == SDL_BUTTON_RIGHT) want_back = true;
            }
            input.handle(e);
        }
        if (done) break;
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        input.collect(SDL_GetTicksNS(), lanes, buttons);
        for (const ButtonInput& b : buttons)
            if (b.down && b.button >= kKey1 && b.button <= kKey7) want_skip = true;
        if (input.button_held(kStart) && input.button_held(kSelect)) want_back = true;
        if (!timers.on(1) && t > sk->startinput) timers.set(1, t), timers.set(11, t);  // LR2: 入力受付と同時に 11 も（Main.cpp:1469）

        // 同じフレームで重なったら、#SCENETIME → 飛ばす → 戻る の順に見る（LR2 と同じ）
        if (phase == Phase::Wait) {
            if (t > sk->scenetime) {
                done = true;
            } else if (want_skip && t > sk->startinput && t > sk->ignore_input) {
                timers.set(3, t), phase = Phase::Skip;
            } else if (want_back && t > sk->startinput) {
                timers.set(2, t), phase = Phase::Back;
                for (int id = 0; id < kSysSoundCount; ++id) app.mixer.stop_system(id);
            }
        } else if (phase == Phase::Back) {
            if (sk->fadeout == 0 || timers.lapse(2, t) > sk->fadeout) result = DecideResult::Back, done = true;
        } else if (phase == Phase::Skip) {
            if (sk->close == 0 || timers.lapse(3, t) > sk->close) done = true;
        }
        if (done) break;

        PlaySkinState st;
        st.now_ms = t;
        st.timers = &timers;
        st.option = [&](int o) {
            if (o == 194 || o == 195) return has_back == (o == 195);
            return ctx.option(o);
        };
        st.number = [&](int id) { return ctx.number(id); };
        st.bargraph = [&](int ty, double& v, double& m) { return ctx.bargraph(ty, v, m); };
        st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx.slider(ty, v, lo, hi); };
        st.text = [&](int id) { return ctx.text(id); };
        st.button = [&](int ty) { return ctx.button(ty); };
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        app.decide_renderer->draw_select(*sk, st);
        if (const char* shot = dev_env("BMJ_DECIDE_SHOT"); shot && t > 1500) {
            // 開発用：1 枚保存して戻る
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            result = DecideResult::Back, done = true;
        }
        frame_present(app.renderer);
        SDL_DelayPrecise(2'000'000);
    }
    for (int id : {100, 101, 102}) app.decide_renderer->set_image_override(id, nullptr);
    for (SDL_Texture* tex : {stage, back, banner})
        if (tex) SDL_DestroyTexture(tex);
    return result;
}

}  // namespace bmj
