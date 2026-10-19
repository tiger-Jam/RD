#include "app/skin_select_scene.h"

#include <algorithm>
#include <cmath>

#include "app/select_context.h"
#include "audio/system_sound.h"
#include "render/skin_play.h"

namespace bmj {

namespace {

// 選んでいるスキンのプレビュー（LR2: MakeSkinPreview / DrawSkinPreview）。スキンを読み込んで動かし、
// 画像 105 の大きさの絵に縮めて描く。サウンドセットは #INFORMATION の 4 番目の欄の画像（無ければ黒）
class SkinPreview {
public:
    SkinPreview(App& app, int w, int h) : app_(app), renderer_(app.renderer) {
        tex_ = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    }
    ~SkinPreview() {
        if (tex_) SDL_DestroyTexture(tex_);
        if (thumb_) SDL_DestroyTexture(thumb_);
    }
    SDL_Texture* texture() const { return tex_; }

    // 選んでいるスキンが変わったら読み直す
    void select(const SkinEntry* e, int type) {
        // カスタマイズを変えたときも読み直す
        std::string want = e ? e->path : "";
        if (e)
            if (auto it = app_.cfg.skin_custom.find(e->path); it != app_.cfg.skin_custom.end())
                for (int v : it->second) want += "," + std::to_string(v);
        if (want == path_ && type == type_) return;
        path_ = want, type_ = type;
        skin_.reset();
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
        SkinHeader h = e->header;
        if (auto it = app_.cfg.skin_custom.find(e->path); it != app_.cfg.skin_custom.end())
            for (size_t i = 0; i < it->second.size() && i < h.customs.size(); ++i)
                if (it->second[i] >= 0 && it->second[i] < static_cast<int>(h.customs[i].labels.size())) h.customs[i].selected = it->second[i];
        auto sk = std::make_unique<Lr2Skin>();
        std::string err;
        if (!load_lr2_skin(app_.lr2_root, e->path, h, ctx_.load_options(), *sk, err)) return;
        if (!h.has_resolution) sk->width = 640, sk->height = 480;
        renderer_.upload(*sk);
        skin_ = std::move(sk);
        ctx_.skin = skin_.get();
    }

    // 1 フレーム分を描く（タイマー 1・40・41 は、LR2 と同じく時間が来たら立てる）
    void draw() {
        if (!tex_) return;
        SDL_Renderer* r = app_.renderer;
        SDL_SetRenderTarget(r, tex_);
        float tw = 0, th = 0;
        SDL_GetTextureSize(tex_, &tw, &th);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        if (thumb_) {
            SDL_FRect d{0, 0, tw, th};
            SDL_RenderTexture(r, thumb_, nullptr, &d);
        } else if (skin_) {
            SDL_SetRenderLogicalPresentation(r, skin_->width, skin_->height, SDL_LOGICAL_PRESENTATION_STRETCH);
            double t = (SDL_GetTicksNS() - start_ns_) / 1e6;
            if (!timers_.on(1) && t > skin_->startinput) timers_.set(1, t);
            if (!timers_.on(40) && t > skin_->loadstart + skin_->loadend) timers_.set(40, t);
            if (timers_.on(40) && !timers_.on(41) && timers_.lapse(40, t) > skin_->playstart) timers_.set(41, t);
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
            if (type_ <= 4) renderer_.draw_play(*skin_, st);
            else if (type_ == 7 || type_ == 15) renderer_.draw_result(*skin_, st);
            else renderer_.draw_select(*skin_, st);
            SDL_SetRenderLogicalPresentation(r, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        }
        SDL_SetRenderTarget(r, nullptr);
    }

private:
    App& app_;
    SkinRenderer renderer_;
    SDL_Texture* tex_ = nullptr;
    SDL_Texture* thumb_ = nullptr;
    std::unique_ptr<Lr2Skin> skin_;
    PlayContext ctx_;
    SkinTimers timers_;
    std::string path_;
    int type_ = -1;
    uint64_t start_ns_ = 0;
};

}  // namespace

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
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(app.renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
    // スキンがマウスカーソルを描くなら OS のカーソルは隠す。描かないなら OS のカーソルを出す
    bool skin_cursor = skin.mousecursor.src.valid && !skin.mousecursor.dst.empty();
    if (skin_cursor) SDL_HideCursor();
    else SDL_ShowCursor();
    while (!done) {
        double t = now_ms();
        if (!timers.on(1) && t > skin.startinput) timers.set(1, t);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) timers.set(2, t);
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                SDL_ConvertEventToRenderCoordinates(app.renderer, &e);
            if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y, mouse_moved_ms = t;
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
                            // サウンドセットはその場で読み直す（選曲の BGM も新しい音で鳴らし直す）
                            if (catalog.selected_type() == 10 && ty == 190) {
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
