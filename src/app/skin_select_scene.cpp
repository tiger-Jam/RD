#include "app/skin_select_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/select_context.h"
#include "audio/system_sound.h"
#include "chart/chart_loader.h"
#include "game/judge.h"
#include "render/skin_play.h"
#include "render/frame_hooks.h"
#include "util/dev.h"

namespace bmj {

namespace {

// 選んでいるスキンのプレビュー（LR2: MakeSkinPreview / DrawSkinPreview）。スキンを読み込んで動かし、
// 画像 105 の大きさの絵に縮めて描く。サウンドセットは #INFORMATION の 4 番目の欄の画像（無ければ黒）。
// 7 鍵・5 鍵のプレースキンは、LR2 と同じく見本の譜面（LR2files/Config/sample_7.bme・sample_5.bme）をオートで流し、
// 終わったら頭から繰り返す（ノーツ・ボム・キーフラッシュ・判定の絵が見える。音は鳴らさない）
class SkinPreview {
public:
    SkinPreview(App& app, int w, int h) : app_(app), renderer_(app.renderer) {
        tex_ = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    }
    ~SkinPreview() {
        if (tex_) SDL_DestroyTexture(tex_);
        if (thumb_) SDL_DestroyTexture(thumb_);
        if (full_) SDL_DestroyTexture(full_);
    }
    SDL_Texture* texture() const { return tex_; }
    // 次の select で必ず読み直す（スキンのファイルを書き換えたとき）
    void invalidate() { path_.clear(), type_ = -1; }

    // 選んでいるスキンが変わったら読み直す
    void select(const SkinEntry* e, int type) {
        // カスタマイズを変えたときも読み直す
        std::string want = e ? e->path : "";
        if (e)
            if (auto it = app_.cfg.skin_custom.find(e->path); it != app_.cfg.skin_custom.end())
                want += "," + format_skin_custom(it->second);
        // ほかのスキンの部品を変えたときも
        if (e)
            if (auto it = app_.cfg.skin_parts.find(e->path); it != app_.cfg.skin_parts.end()) {
                want += it->second.screen_scale ? "|screen" : "|lane";
                for (const auto& [k, v] : it->second.from) want += "|" + k + "=" + v;
                for (const auto& [k, c] : it->second.custom)
                    for (const auto& [i, name] : c) want += "|" + k + "." + std::to_string(i) + "=" + name;
            }
        if (want == path_ && type == type_) return;
        path_ = want, type_ = type;
        skin_.reset();
        demo_judge_.reset(), demo_.reset(), demo_start_ = -1;
        ctx_.skin = nullptr, ctx_.chart = nullptr;  // 捨てたものを読み込みの条件（ctx_.load_options）で見ないように
        if (thumb_) SDL_DestroyTexture(thumb_), thumb_ = nullptr;
        start_ns_ = SDL_GetTicksNS();
        timers_ = SkinTimers();
        timers_.set(0, 0);
        if (!e) return;
        if (type == 10) {
            // サウンドセット：サムネイルの画像
            if (!e->header.thumbnail.empty()) {
                std::string p = resolve_lr2_path(app_.lr2_root, e->header.thumbnail, false);
                SkinImage img;
                static const uint8_t black[3] = {0, 0, 0};
                if (!p.empty() && load_skin_image(p, img, black, false)) thumb_ = renderer_.create_texture(img);
            }
            return;
        }
        // 見本の譜面は先に読む（スキンの #IF に効く譜面の条件を、遊ぶときと同じに）
        if (type == 0 || type == 1) {
            const std::string sample =
                resolve_lr2_path(app_.lr2_root, type == 1 ? "LR2files/Config/sample_5.bme" : "LR2files/Config/sample_7.bme", false);
            BmsParseOptions popt;
            popt.random_seed = 1;
            if (!sample.empty())
                if (BmsParseResult r = parse_chart_file(sample, popt); r.ok) demo_ = std::make_unique<Chart>(std::move(r.chart));
            ctx_.chart = demo_.get();
        }
        SkinHeader h = e->header;
        apply_skin_custom(app_.cfg, e->path, h);
        auto sk = std::make_unique<Lr2Skin>();
        std::string err;
        if (!load_lr2_skin(app_.lr2_root, e->path, h, ctx_.load_options(), *sk, err)) return;
        if (!h.has_resolution) guess_resolution(*sk);  // 本体と同じく、解像度の書いていないスキンは中身から推定する
        if (type == 0 || type == 1) app_.apply_skin_mods(*sk, e->path, ctx_.load_options());  // ほかのスキンの部品と色（遊ぶときと同じに見せる）
        renderer_.upload(*sk);
        // スキンはスキン本来の大きさの絵に描いてから、プレビューの枠の大きさに縮めて貼る
        float fw = 0, fh = 0;
        if (full_) SDL_GetTextureSize(full_, &fw, &fh);
        if (!full_ || fw != sk->width || fh != sk->height) {
            if (full_) SDL_DestroyTexture(full_);
            full_ = SDL_CreateTexture(app_.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, sk->width, sk->height);
            if (full_) SDL_SetTextureScaleMode(full_, SDL_SCALEMODE_LINEAR);
        }
        skin_ = std::move(sk);
        ctx_.skin = skin_.get();
    }

    // 1 フレーム分を描く（タイマー 1・40・41 は、LR2 と同じく時間が来たら立てる）
    void draw() {
        if (!tex_) return;
        SDL_Renderer* r = app_.renderer;
        SDL_Texture* prev_target = SDL_GetRenderTarget(r);  // 画面の描き先（frame_target_set）に戻す
        SDL_SetRenderTarget(r, tex_);
        float tw = 0, th = 0;
        SDL_GetTextureSize(tex_, &tw, &th);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        if (thumb_) {
            SDL_FRect d{0, 0, tw, th};
            SDL_RenderTexture(r, thumb_, nullptr, &d);
        } else if (skin_ && full_) {
            SDL_SetRenderTarget(r, full_);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            double t = (SDL_GetTicksNS() - start_ns_) / 1e6;
            if (!timers_.on(1) && t > skin_->startinput) timers_.set(1, t);
            if (!timers_.on(40) && t > skin_->loadstart + skin_->loadend) timers_.set(40, t);
            if (timers_.on(40) && !timers_.on(41) && timers_.lapse(40, t) > skin_->playstart) timers_.set(41, t);
            if (demo_ && timers_.on(41)) play_demo(t);
            ctx_.timers = timers_;
            ctx_.now_ms = t;
            PlaySkinState st;
            st.now_ms = t;
            st.timers = &timers_;
            st.option = [&](int o) { return ctx_.option(o); };
            st.number = [&](int id) { return ctx_.number(id); };
            st.bargraph = [&](int ty, double& v, double& m) { return ctx_.bargraph(ty, v, m); };
            st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx_.slider(ty, v, lo, hi); };
            st.text = [&](int id) { return ctx_.text(id); };
            st.button = [&](int ty) { return ctx_.button(ty); };
            if (demo_judge_) {
                const int64_t us = static_cast<int64_t>((t - demo_start_) * 1000);
                st.chart = demo_.get(), st.judge = demo_judge_.get();
                st.notes_visible = true, st.autoplay = true;
                st.render_beat = demo_->pos_at(us);
                st.hi_speed = app_.cfg.hi_speed, st.base_speed = app_.cfg.base_speed;
                st.judge_draw = ctx_.judge_draw;
                st.combo_draw = demo_judge_->stats().combo;
                st.gauge_hp = demo_judge_->play_hp();
            }
            if (type_ <= 4) renderer_.draw_play(*skin_, st);
            else if (type_ == 7 || type_ == 15) renderer_.draw_result(*skin_, st);
            else renderer_.draw_select(*skin_, st);
            // 縦横の比を保って枠に収める（16:9 のスキンを 4:3 の枠に出すときは、上下に黒い帯）
            SDL_SetRenderTarget(r, tex_);
            const float k = std::min(tw / skin_->width, th / skin_->height);
            const float dw = skin_->width * k, dh = skin_->height * k;
            SDL_FRect d{(tw - dw) / 2, (th - dh) / 2, dw, dh};
            SDL_RenderTexture(r, full_, nullptr, &d);
        }
        SDL_SetRenderTarget(r, prev_target);
    }

private:
    // 見本の譜面を t（シーンの時刻）まで進め、判定でボム・キーフラッシュ・判定の絵のタイマーを立てる（遊ぶときと同じ）
    void play_demo(double t) {
        if (!demo_judge_ || t - demo_start_ > demo_->end_us / 1000.0 + 2000) {
            JudgeConfig jc;
            jc.autoplay = true;
            demo_judge_ = std::make_unique<JudgeEngine>(*demo_, jc);
            demo_start_ = t;
        }
        demo_judge_->advance(static_cast<int64_t>((t - demo_start_) * 1000));
        for (const JudgeEvent& je : demo_judge_->events()) {
            if (je.mine) continue;
            const double at = demo_start_ + je.time_us / 1000.0;
            const int j = je.judge >= 100 ? je.judge - 100 : je.judge;
            if (je.judge >= 100) {
                if (j >= kGood) timers_.set(70 + je.lane, at);
                continue;
            }
            if (je.ln_start && j >= kGood) timers_.set(70 + je.lane, at);
            if (j >= kGreat && !je.ln_release) timers_.set(50 + je.lane, at);
            ctx_.judge_draw = j;
            timers_.set(46, at);
            timers_.set(100 + je.lane, at);
            timers_.reset(120 + je.lane);
        }
        demo_judge_->events().clear();
        for (int lane = 0; lane < kLaneCount; ++lane) {
            if (!demo_judge_->lane_ln_holding(lane)) timers_.reset(70 + lane);
            // 自動演奏の鍵盤の光は 80ms で消す（遊ぶときと同じ）
            if (timers_.lapse(50 + lane, t) >= 80 && timers_.lapse(100 + lane, t) > 0) {
                timers_.reset(100 + lane);
                timers_.set(120 + lane, t);
            }
        }
    }

    App& app_;
    SkinRenderer renderer_;
    SDL_Texture* tex_ = nullptr;
    SDL_Texture* thumb_ = nullptr;
    SDL_Texture* full_ = nullptr;  // スキン本来の大きさの描き先
    std::unique_ptr<Lr2Skin> skin_;
    std::unique_ptr<Chart> demo_;               // 見本の譜面（7 鍵・5 鍵のプレースキン）
    std::unique_ptr<JudgeEngine> demo_judge_;   // その自動演奏
    double demo_start_ = -1;                    // 見本の譜面の頭のシーンの時刻
    PlayContext ctx_;
    SkinTimers timers_;
    std::string path_;
    int type_ = -1;
    uint64_t start_ns_ = 0;
};

}  // namespace

bool run_skin_select(App& app, SkinCatalog& catalog) {
    // 入るたびにスキンの一覧を集め直す（LR2files/Theme に新しいスキンを入れたら、曲を読み直さずに選べる）
    auto rescan = [&] {
        const int type = catalog.selected_type();
        catalog.scan(app.lr2_root);
        catalog.select_type(type, app.cfg);
    };
    rescan();
    SelectContext ctx;
    ctx.cfg = &app.cfg;
    ctx.catalog = &catalog;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = ctx.option(i);
    SkinRenderer renderer(app.renderer);
    // スキンセレクトのスキン（設定に無ければ LR2 の既定）
    std::string path = !app.cfg.skinselect_skin.empty() ? app.cfg.skinselect_skin
                                                        : resolve_lr2_path(app.lr2_root, "LR2files/Theme/LR2/SkinSelect/skinselect.lr2skin", false);
    SkinHeader header;
    Lr2Skin skin;
    std::string err;
    if (!path.empty() && read_skin_header(app.lr2_root, path, header)) apply_skin_custom(app.cfg, path, header);
    if (path.empty() || header.type < 0 || !load_lr2_skin(app.lr2_root, path, header, ops, skin, err)) {
        std::printf("スキンセレクトのスキンを読めない: %s\n", err.c_str());
        return false;
    }
    if (!header.has_resolution) skin.width = 640, skin.height = 480;
    renderer.upload(skin);
    ctx.skin = &skin;
    app.set_view(skin.width, skin.height);
    // プレビューの絵は、画像 105 の切り出しの大きさで作る
    int pw = 640, ph = 480;
    for (const SkinObject& o : skin.image_objs)
        if (o.src.gr == 105 && o.src.w > 0 && o.src.h > 0) pw = o.src.w, ph = o.src.h;
    SkinPreview preview(app, pw, ph);
    renderer.set_image_override(105, preview.texture());

    SkinTimers timers;
    timers.set(0, 0);
    const uint64_t start = SDL_GetTicksNS();
    auto now_ms = [&] { return (SDL_GetTicksNS() - start) / 1e6; };
    bool select_changed = false, done = false;
    const SkinObject* dragging = nullptr;
    float mouse_x = -1, mouse_y = -1;
    double mouse_moved_ms = 0;
    int dev_key = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(app.renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
    // スキンがマウスカーソルを描くなら OS のカーソルは隠す。描かないなら OS のカーソルを出す
    bool skin_cursor = skin.mousecursor.src.valid && !skin.mousecursor.dst.empty();
    if (skin_cursor) SDL_HideCursor();
    else SDL_ShowCursor();
    while (!done) {
        double t = now_ms();
        if (!timers.on(1) && t > skin.startinput) timers.set(1, t), timers.set(11, t);  // LR2: 入力受付と同時に 11 も
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) timers.set(2, t);
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                SDL_ConvertEventToRenderCoordinates(app.renderer, &e);
            if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y, mouse_moved_ms = t;
            // ESC・右クリックで閉じる（#FADEOUT のあと選曲へ）
            // F8：スキンの一覧と、選んでいるスキンのプレビューを読み直す（スキンを足したり書き換えたりしたとき。曲は読み直さない）
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F8 && !e.key.repeat) {
                rescan();
                preview.invalidate();
                app.mixer.play_system(kSysOptionChange);
                std::printf("スキンの一覧を読み直した\n");
            }
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
                            // サウンドセットはその場で読み直す（選曲の BGM も新しい音で鳴らし直す）
                            if (catalog.selected_type() == 10) {  // サウンドセットか、その中の選択（選曲 BGM など）を替えた
                                app.load_sound_set();
                                app.mixer.play_system(kSysSelect, true);
                            }
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

        // 開発用：BMJ_SKINSELECT_KEYS の文字を 1 フレームに 1 つずつ。'0'〜'4' はその行を次へ、'j' は 1 行下へ送る
        if (const char* keys = dev_env("BMJ_SKINSELECT_KEYS"); keys && keys[dev_key] && t > 300) {
            const char k = keys[dev_key++];
            if (k >= '0' && k <= '4') catalog.step_custom(app.cfg, k - '0', 1);
            else if (k == 'j') catalog.set_scroll(catalog.scroll() + 1, app.cfg);
        }
        preview.select(catalog.current(app.cfg), catalog.selected_type());
        preview.draw();
        PlaySkinState st;
        st.now_ms = t;
        st.timers = &timers;
        st.option = [&](int o) { return ctx.option(o); };
        st.number = [&](int id) { return ctx.number(id); };
        st.bargraph = [&](int ty, double& v, double& m) { return ctx.bargraph(ty, v, m); };
        st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx.slider(ty, v, lo, hi); };
        st.text = [&](int id) { return ctx.text(id); };
        st.button = [&](int ty) { return ctx.button(ty); };
        // マウスを乗せたときの絵とマウスカーソル（曲リストは無い）
        SelectSkinState ss;
        ss.mouse_x = mouse_x, ss.mouse_y = mouse_y;
        ss.mouse_idle_ms = t - mouse_moved_ms;
        st.select = &ss;
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        renderer.draw_select(skin, st);
        const char* wait = dev_env("BMJ_SKINSELECT_WAIT");
        if (const char* shot = dev_env("BMJ_SKINSELECT_SHOT"); shot && t > (wait ? std::atof(wait) : 800)) {
            // 開発用：1 枚保存して閉じる（BMJ_SKINSELECT_WAIT ms 待ってから。既定 800）
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            done = true;
        }
        frame_present(app.renderer);
        SDL_DelayPrecise(2'000'000);
    }
    return select_changed;
}

}  // namespace bmj
