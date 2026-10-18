#include "app/app.h"

#include <cstdio>

#include "audio/system_sound.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace bmj {

namespace {

// LR2のフォルダ（LR2files があるところ）を探す
std::string find_lr2_root(const std::string& configured, const char* argv0) {
    std::vector<std::string> cands;
    if (!configured.empty()) cands.push_back(configured);
    if (const char* env = std::getenv("BMJ_LR2_ROOT")) cands.push_back(env);
    cands.push_back(".");
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    cands.push_back((exe / "..").string());
    cands.push_back((exe / ".." / "参考に" / "LR2beta3 2").string());  // 開発用
    for (const std::string& c : cands)
        if (!resolve_lr2_path(c, "LR2files", false).empty()) return c;
    return {};
}

// #RESOLUTION の無いスキンは、画面全体を覆うDST（0,0 から 640x480 / 1280x720 / 1920x1080）を探して解像度を推定する。
// 見つからなければSD（BMjoyの拡張。OpenLR2は設定の既定値を使う）
void guess_resolution(Lr2Skin& sk) {
    static const int sizes[3][2] = {{640, 480}, {1280, 720}, {1920, 1080}};
    int hits[3] = {};
    for (auto& o : sk.image_objs)
        for (const SkinDstKey& k : o.dst.keys)
            for (int i = 0; i < 3; ++i)
                if (k.x == 0 && k.y == 0 && k.w == sizes[i][0] && k.h == sizes[i][1]) hits[i]++;
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (hits[i] > hits[best]) best = i;
    sk.width = sizes[best][0];
    sk.height = sizes[best][1];
}

}  // namespace

App::~App() {
    skin_renderer.reset();
    result_renderer.reset();
    select_renderer.reset();
    decide_renderer.reset();
    mixer.close();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
}

bool App::init(const char* argv0) {
    lr2_root = find_lr2_root(cfg.lr2_root, argv0);
    if (keycfg.load(keyconfig_path())) std::printf("キーコンフィグ: %s\n", keyconfig_path().c_str());
    std::printf("ウィンドウを作っています\n");
    int ww, wh;
    window_size(640, 480, ww, wh);
    window = SDL_CreateWindow("BMjoy", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "ウィンドウを作れない: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(renderer, cfg.vsync ? 1 : 0);
    if (cfg.fullscreen) SDL_SetWindowFullscreen(window, true);
    // 別のプロセスから起動されたときでも、前面に出てキーボードの入力を受けられるようにする
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    SDL_RaiseWindow(window);
    std::printf("描画: %s\n", SDL_GetRendererName(renderer));
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
        if (m->refresh_rate > 0) refresh_hz = m->refresh_rate;
    skin_renderer = std::make_unique<SkinRenderer>(renderer);
    result_renderer = std::make_unique<SkinRenderer>(renderer);
    select_renderer = std::make_unique<SkinRenderer>(renderer);
    decide_renderer = std::make_unique<SkinRenderer>(renderer);
    set_view(640, 480);
    std::printf("音声デバイスを開いています\n");
    // 音の出力（LR2 SETUP の OUTPUT TYPE とバッファ数）。排他モードが使えなければ共有モードになる
    if (!mixer.open(0, cfg.audio_buffer > 0 ? static_cast<uint32_t>(cfg.audio_buffer) : (cfg.audio_exclusive ? 64 : 128),
                    cfg.audio_exclusive)) {
        std::fprintf(stderr, "音声デバイスを開けない\n");
        return false;
    }
    {
        // システム音（LR2のサウンドセット）
        std::string ss = cfg.sound_set.empty() ? resolve_lr2_path(lr2_root, "LR2files/Sound/lr2.lr2ss", false) : cfg.sound_set;
        if (!lr2_root.empty() && !ss.empty()) {
            std::vector<Sample> sounds = load_system_sounds(lr2_root, ss, mixer.sample_rate());
            int n = 0;
            for (const Sample& smp : sounds) n += !smp.pcm.empty();
            std::printf("システム音: %d / %zu（%s）\n", n, sounds.size(), ss.c_str());
            mixer.set_system_sounds(std::move(sounds));
        }
    }
    if (!db.open(song_db_path())) std::fprintf(stderr, "曲データベースを開けない: %s\n", song_db_path().c_str());
    double estimate = mixer.output_latency_us() / 1000.0;
    // 実際の遅延を測って、曲の時計（判定とノーツの位置）に使う（Windows のみ）
    bool measured = mixer.calibrate_latency();
    std::printf("音声: %s／%u Hz／%u フレーム×%u、出力遅延 %.1f ms（%s%s）／画面 %.0f Hz\n", mixer.exclusive() ? "排他" : "共有",
                mixer.sample_rate(), mixer.period_frames(), mixer.periods(), mixer.output_latency_us() / 1000.0,
                measured ? "実測。見積もりは " : "見積もり", measured ? (std::to_string(static_cast<int>(estimate * 10) / 10.0).substr(0, 4) + " ms").c_str() : "",
                refresh_hz);
    if (cfg.audio_exclusive && !mixer.exclusive()) std::printf("排他モードで開けなかったので、共有モードで鳴らしています\n");
    return true;
}

// ウィンドウの大きさ。解像度（SD/HD/FHD）の指定があればその大きさ、無ければスキンの大きさ（SD は 2 倍）。
// スキンは自分の大きさで描いて、ウィンドウいっぱいに拡大する（縦横比は保つ）
void App::window_size(int skin_w, int skin_h, int& w, int& h) const {
    if (cfg.window_w > 0 && cfg.window_h > 0) {
        w = cfg.window_w, h = cfg.window_h;
        return;
    }
    int scale = cfg.window_scale > 0 ? cfg.window_scale : 100;
    if (cfg.skin_res == "sd") w = 640, h = 480;
    else if (cfg.skin_res == "hd") w = 1280, h = 720;
    else if (cfg.skin_res == "fhd") w = 1920, h = 1080;
    else {
        w = skin_w, h = skin_h;
        if (cfg.window_scale <= 0 && skin_w <= 640) scale = 200;
    }
    w = w * scale / 100;
    h = h * scale / 100;
}

void App::set_view(int w, int h, bool resize_window) {
    if (resize_window && (w != view_w || h != view_h)) {
        // ウィンドウの大きさもスキンに合わせる（SDは2倍）
        int ww, wh;
        window_size(w, h, ww, wh);
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(window, ww, wh);
    }
    view_w = w;
    view_h = h;
    // スキン解像度で描いて拡大する（LR2と同じ見た目）
    SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

namespace {

// .lr2skin を読む。configured が空なら LR2 の既定のスキン（default_rel）を使う
std::unique_ptr<Lr2Skin> load_skin(App& app, const std::string& configured, const char* default_rel,
                                   const std::array<bool, 1000>& options, SkinRenderer& renderer) {
    std::string skin_path = configured.empty() ? resolve_lr2_path(app.lr2_root, default_rel, false) : configured;
    SkinHeader header;
    if (app.lr2_root.empty() || skin_path.empty() || !read_skin_header(app.lr2_root, skin_path, header)) return nullptr;
    if (auto it = app.cfg.skin_custom.find(skin_path); it != app.cfg.skin_custom.end())
        for (size_t i = 0; i < it->second.size() && i < header.customs.size(); ++i)
            if (it->second[i] >= 0 && it->second[i] < static_cast<int>(header.customs[i].labels.size()))
                header.customs[i].selected = it->second[i];
    std::printf("スキン: %s（%s）\n", header.title.c_str(), skin_path.c_str());
    for (size_t i = 0; i < header.customs.size(); ++i) {
        const SkinCustomItem& c = header.customs[i];
        std::string sel = c.selected < static_cast<int>(c.labels.size()) ? c.labels[c.selected] : "?";
        std::printf("  [%zu] %s = %s\n", i, c.title.c_str(), sel.c_str());
    }
    auto skin = std::make_unique<Lr2Skin>();
    std::string err;
    if (!load_lr2_skin(app.lr2_root, skin_path, header, options, *skin, err)) {
        std::printf("スキンを読めない: %s\n", err.c_str());
        return nullptr;
    }
    if (!header.has_resolution) guess_resolution(*skin);
    for (const auto& w : skin->warnings)
        if (w.find(".dummy") == std::string::npos) std::printf("  スキン警告: %s\n", w.c_str());
    renderer.upload(*skin);
    return skin;
}

}  // namespace

void App::load_play_skin(const std::array<bool, 1000>& options) {
    play_skin = load_skin(*this, cfg.play_skin, "LR2files/Theme/LR2/Play/play_7.lr2skin", options, *skin_renderer);
    if (!play_skin) std::printf("LR2スキンが見つからないので、仮の画面で遊ぶ（--lr2-root でLR2のフォルダを指定できる）\n");
}

void App::load_select_skin(const std::array<bool, 1000>& options) {
    if (cfg.select_skin == "none") {
        select_skin.reset();
        return;
    }
    select_skin = load_skin(*this, cfg.select_skin, "LR2files/Theme/LR2/Select/select.lr2skin", options, *select_renderer);
}

void App::load_decide_skin(const std::array<bool, 1000>& options) {
    if (cfg.decide_skin == "none") {
        decide_skin.reset();
        return;
    }
    decide_skin = load_skin(*this, cfg.decide_skin, "LR2files/Theme/LR2/Decide/decide.lr2skin", options, *decide_renderer);
}

void App::load_result_skin(const std::array<bool, 1000>& options) {
    courseresult_skin.reset();
    result_skin = load_skin(*this, cfg.result_skin, "LR2files/Theme/LR2/Result/result.lr2skin", options, *result_renderer);
}

// コースリザルトは、リザルトと同じ描画器を使う（同時には出さないので）
void App::load_courseresult_skin(const std::array<bool, 1000>& options) {
    result_skin.reset();
    courseresult_skin = load_skin(*this, cfg.courseresult_skin, "LR2files/Theme/LR2/CourseResult/courseresult.lr2skin", options,
                                  *result_renderer);
}

}  // namespace bmj
