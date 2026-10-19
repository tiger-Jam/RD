#include "app/keyconfig_skin_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "app/keyconfig_editor.h"
#include "audio/system_sound.h"
#include "input/joystick_init.h"
#include "render/frame_hooks.h"
#include "render/keyconfig_scene.h"
#include "render/text_cache.h"

namespace bmj {

namespace {

constexpr const char* kDefaultSkin = "LR2files/Theme/LR2/KeyConfig/keyconfig.lr2skin";

// 開発用：BMJ_KEYCONFIG_SKIN_KEYS の文字で操作する（数字 1-7 / s / S / t / l = 1〜7 鍵・皿の 2 方向・START・SELECT を選ぶ、
// a-h = スロット 1〜8 を選ぶ、z = Z キーを割り当てる、x = Delete、r = F1、C = F2、_ = 少し待つ）
void dev_step(char k, KeyconfigEditor& ed, const std::function<void(const Binding&)>& assign, const std::function<void()>& erase,
              const std::function<void()>& reset, const std::function<void()>& clear) {
    if (k >= '1' && k <= '7') ed.click(100 + (k - '0'), 1);
    else if (k == 's') ed.click(110, 1);
    else if (k == 'S') ed.click(111, 1);
    else if (k == 't') ed.click(112, 1);
    else if (k == 'l') ed.click(113, 1);
    else if (k >= 'a' && k <= 'h') ed.click(150 + (k - 'a'), 1);
    else if (k == 'z') assign({Binding::Type::Key, SDL_SCANCODE_Z, 0});
    else if (k == 'x') erase();
    else if (k == 'r') reset();
    else if (k == 'C') clear();
}

KeyconfigExit run_with_skin(App& app, Lr2Skin& skin, SkinRenderer& renderer) {
    app.set_view(skin.width, skin.height);
    KeyconfigEditor ed(app.keycfg);
    SelectContext base;  // 時刻などのキーコンフィグ以外の値
    base.cfg = &app.cfg;
    base.skin = &skin;
    const std::string path = keyconfig_path();
    auto save = [&] {
        if (!app.keycfg.save(path)) std::printf("キーコンフィグを保存できない: %s\n", path.c_str());
    };
    // 割り当てるたびに保存する（LR2 と同じ）
    auto assign = [&](const Binding& b) {
        if (ed.assign(b)) save(), app.mixer.play_system(kSysOptionChange);
    };
    auto erase = [&] {
        if (ed.erase()) save(), app.mixer.play_system(kSysOptionChange);
    };
    auto reset = [&] { ed.reset_default(), save(); };
    auto clear = [&] { ed.clear_all(), save(); };

    SkinTimers timers;
    timers.set(0, 0);
    const uint64_t start = SDL_GetTicksNS();
    auto now_ms = [&] { return (SDL_GetTicksNS() - start) / 1e6; };
    float mouse_x = -1, mouse_y = -1;
    double mouse_moved_ms = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(app.renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
    bool skin_cursor = skin.mousecursor.src.valid && !skin.mousecursor.dst.empty();
    if (skin_cursor) SDL_HideCursor();
    else SDL_ShowCursor();
    // 軸は、中央付近から端まで倒したときだけ割り当てる（機器・軸ごとの前の値）
    std::map<std::pair<uint32_t, int>, int16_t> axis_prev;
    auto option_of = [&](int o) {
        bool neg = o < 0;
        int a = neg ? -o : o;
        bool v;
        if (!ed.option(a, v)) v = base.option(a);
        return neg ? !v : v;
    };
    const char* dev_keys = std::getenv("BMJ_KEYCONFIG_SKIN_KEYS");
    size_t dev_pos = 0;
    double dev_next = 600;
    KeyconfigExit result = KeyconfigExit::Back;
    bool done = false;
    while (!done) {
        double t = now_ms();
        if (!timers.on(1) && t > skin.startinput) timers.set(1, t);
        const bool fading = timers.on(2);
        const bool accept = timers.on(1) && !fading;
        auto start_fade = [&] {
            if (!timers.on(2)) timers.set(2, t);
        };
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) result = KeyconfigExit::Quit, done = true;
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                SDL_ConvertEventToRenderCoordinates(app.renderer, &e);
            if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y, mouse_moved_ms = t;
            // ESC・右クリックで閉じる（#FADEOUT のあと選曲へ）
            if ((e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) ||
                (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT)) {
                start_fade();
                continue;
            }
            if (!accept) continue;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Scancode sc = e.key.scancode;
                if (sc == SDL_SCANCODE_F1) reset();
                else if (sc == SDL_SCANCODE_F2) clear();
                else if (sc == SDL_SCANCODE_DELETE) erase();
                else if (!keyconfig_reserved(sc)) assign({Binding::Type::Key, sc, 0});
            } else if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
                assign({Binding::Type::JoyButton, e.jbutton.button, 0});
            } else if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && e.jhat.value) {
                int v = e.jhat.value;
                int dir = v & SDL_HAT_UP ? SDL_HAT_UP : v & SDL_HAT_DOWN ? SDL_HAT_DOWN : v & SDL_HAT_LEFT ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                assign({Binding::Type::JoyHat, e.jhat.hat, dir});
            } else if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
                int a = e.jaxis.axis;
                auto [it, first] = axis_prev.try_emplace({e.jaxis.which, a}, static_cast<int16_t>(0));
                int16_t prev = it->second;
                it->second = e.jaxis.value;
                // アナログ皿に使っている軸は方向として割り当てない
                if (a != app.keycfg.analog_axis && std::abs(e.jaxis.value) > 24000 && std::abs(prev) < 16000 && !first)
                    assign({Binding::Type::JoyAxis, a, e.jaxis.value > 0 ? 1 : -1});
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                float mx = e.button.x, my = e.button.y;
                // スキンのボタン。後に書かれたものが上に描かれるので、後ろから探す
                for (auto o = skin.buttons.rbegin(); o != skin.buttons.rend(); ++o) {
                    if (o->src.op[1] != 1 || o->dst.empty() || !timers.on(o->dst.timer)) continue;
                    if (o->src.op[2] != 0) continue;  // パネルは無い
                    if (!option_of(o->dst.op[0]) || !option_of(o->dst.op[1]) || !option_of(o->dst.op[2])) continue;
                    SkinDraw d = eval_dst(o->dst, timers.lapse(o->dst.timer, t));
                    if (!d.visible) continue;
                    float x0 = std::min(d.x, d.x + d.w), x1 = std::max(d.x, d.x + d.w);
                    float y0 = std::min(d.y, d.y + d.h), y1 = std::max(d.y, d.y + d.h);
                    if (mx < x0 || mx >= x1 || my < y0 || my >= y1) continue;
                    int dir = o->src.op[3] == 1 ? 1 : o->src.op[3] == 2 ? -1 : (mx >= (x0 + x1) / 2 ? 1 : -1);
                    if (ed.click(o->src.op[0], dir)) app.mixer.play_system(kSysOptionChange);
                    break;
                }
            }
        }
        if (done) break;
        if (dev_keys && accept && t > dev_next) {
            if (dev_keys[dev_pos]) {
                char k = dev_keys[dev_pos++];
                if (k == '_') dev_next = t + 200;
                else dev_step(k, ed, assign, erase, reset, clear), dev_next = t + 30;
            }
        }
        if (fading && (skin.fadeout == 0 || timers.lapse(2, t) > skin.fadeout)) break;
        joystick_init().open_new();

        PlaySkinState st;
        st.now_ms = t;
        st.timers = &timers;
        st.option = option_of;
        st.number = [&](int id) { return base.number(id); };
        st.bargraph = [&](int ty, double& v, double& m) { return base.bargraph(ty, v, m); };
        st.slider = [&](int ty, int& v, int& lo, int& hi) { return base.slider(ty, v, lo, hi); };
        st.text = [&](int id) {
            std::string s;
            return ed.text(id, s) ? s : base.text(id);
        };
        st.button = [&](int ty) {
            int v;
            return ed.button(ty, v) ? v : base.button(ty);
        };
        // マウスを乗せたときの絵とマウスカーソル（曲リストは無い）
        SelectSkinState ss;
        ss.mouse_x = mouse_x, ss.mouse_y = mouse_y;
        ss.mouse_idle_ms = t - mouse_moved_ms;
        st.select = &ss;
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        renderer.draw_select(skin, st);
        if (const char* shot = std::getenv("BMJ_KEYCONFIG_SKIN_SHOT");
            shot && t > 800 && (!dev_keys || (!dev_keys[dev_pos] && t > dev_next + 100))) {
            // 開発用：1 枚保存して閉じる
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            done = true;
        }
        frame_hooks_before_present(app.renderer);
        SDL_RenderPresent(app.renderer);
        SDL_DelayPrecise(2'000'000);
    }
    // 選曲に戻るときに読み直す（LR2 と同じ）
    app.keycfg.load(path);
    return result;
}

std::unique_ptr<Lr2Skin> load_keyconfig_skin(App& app, SkinRenderer& renderer) {
    SelectContext ctx;
    ctx.cfg = &app.cfg;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = ctx.option(i);
    ops[400] = true;  // 7 鍵モード
    ops[401] = ops[402] = false;
    return app.load_scene_skin(app.cfg.keyconfig_skin, kDefaultSkin, ops, renderer);
}

}  // namespace

bool run_keyconfig_skin(App& app, KeyconfigExit& exit) {
    SkinRenderer renderer(app.renderer);
    std::unique_ptr<Lr2Skin> skin = load_keyconfig_skin(app, renderer);
    if (!skin) return false;
    exit = run_with_skin(app, *skin, renderer);
    return true;
}

bool open_keyconfig_from_select(App& app, TextCache& text, SelectContext& sctx, const std::function<double()>& scene_ms,
                                bool faded) {
    SkinRenderer renderer(app.renderer);
    std::unique_ptr<Lr2Skin> skin = load_keyconfig_skin(app, renderer);
    if (!skin) {
        // スキンが無ければウィザード
        if (run_key_wizard(app.renderer, text, app.keycfg, keyconfig_path())) app.mixer.play_system(kSysDecide);
        return true;
    }
    // 選曲画面のフェードアウト（タイマー 2）。システム音を止め、オプションの音を鳴らす（LR2 と同じ）
    if (!faded) {
        for (int id = 0; id < kSysSoundCount; ++id) app.mixer.stop_system(id);
        app.mixer.play_system(kSysOptionChange);
    }
    if (const Lr2Skin* sel = faded ? nullptr : app.select_skin.get()) {
        sctx.timers.set(2, scene_ms());
        bool quit = false;
        while (!quit) {
            double t = scene_ms();
            if (sel->fadeout == 0 || sctx.timers.lapse(2, t) > sel->fadeout) break;
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT) quit = true;
            PlaySkinState st;
            st.now_ms = t;
            st.timers = &sctx.timers;
            st.option = [&](int o) { return sctx.option(o); };
            st.number = [&](int id) { return sctx.number(id); };
            st.bargraph = [&](int ty, double& v, double& m) { return sctx.bargraph(ty, v, m); };
            st.slider = [&](int ty, int& v, int& lo, int& hi) { return sctx.slider(ty, v, lo, hi); };
            st.text = [&](int id) { return sctx.text(id); };
            st.button = [&](int ty) { return sctx.button(ty); };
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            SDL_RenderClear(app.renderer);
            app.select_renderer->draw_select(*sel, st);
            frame_hooks_before_present(app.renderer);
            SDL_RenderPresent(app.renderer);
            SDL_DelayPrecise(2'000'000);
        }
        if (quit) return false;
    }
    KeyconfigExit ex = run_with_skin(app, *skin, renderer);
    if (ex == KeyconfigExit::Quit) return false;
    app.mixer.play_system(kSysSelect, true);
    return true;
}

}  // namespace bmj
