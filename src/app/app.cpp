#include "app/app.h"

#include <cstdio>
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
    mixer.close();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
}

bool App::init(const char* argv0) {
    lr2_root = find_lr2_root(cfg.lr2_root, argv0);
    if (keycfg.load(keyconfig_path())) std::printf("キーコンフィグ: %s\n", keyconfig_path().c_str());
    window = SDL_CreateWindow("BMjoy", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "ウィンドウを作れない: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(renderer, cfg.vsync ? 1 : 0);
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
        if (m->refresh_rate > 0) refresh_hz = m->refresh_rate;
    skin_renderer = std::make_unique<SkinRenderer>(renderer);
    set_view(640, 480);
    if (!mixer.open(0, 128)) {
        std::fprintf(stderr, "音声デバイスを開けない\n");
        return false;
    }
    std::printf("音声: %u Hz、出力遅延の推定 %.1f ms／画面 %.0f Hz\n", mixer.sample_rate(),
                mixer.output_latency_us() / 1000.0, refresh_hz);
    return true;
}

void App::set_view(int w, int h, bool resize_window) {
    if (resize_window && (w != view_w || h != view_h)) {
        // ウィンドウの大きさもスキンに合わせる（SDは2倍）
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(window, w <= 640 ? w * 2 : w, w <= 640 ? h * 2 : h);
    }
    view_w = w;
    view_h = h;
    // スキン解像度で描いて拡大する（LR2と同じ見た目）
    SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

void App::load_play_skin(const std::array<bool, 1000>& options) {
    play_skin.reset();
    std::string skin_path = cfg.play_skin.empty()
                                ? resolve_lr2_path(lr2_root, "LR2files/Theme/LR2/Play/play_7.lr2skin", false)
                                : cfg.play_skin;
    SkinHeader header;
    if (lr2_root.empty() || skin_path.empty() || !read_skin_header(lr2_root, skin_path, header)) {
        std::printf("LR2スキンが見つからないので、仮の画面で遊ぶ（--lr2-root でLR2のフォルダを指定できる）\n");
        return;
    }
    if (auto it = cfg.skin_custom.find(skin_path); it != cfg.skin_custom.end())
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
    if (!load_lr2_skin(lr2_root, skin_path, header, options, *skin, err)) {
        std::printf("スキンを読めない: %s\n", err.c_str());
        return;
    }
    if (!header.has_resolution) guess_resolution(*skin);
    if (cfg.skin_res == "sd") skin->width = 640, skin->height = 480;
    else if (cfg.skin_res == "hd") skin->width = 1280, skin->height = 720;
    else if (cfg.skin_res == "fhd") skin->width = 1920, skin->height = 1080;
    for (const auto& w : skin->warnings)
        if (w.find(".dummy") == std::string::npos) std::printf("  スキン警告: %s\n", w.c_str());
    skin_renderer->upload(*skin);
    play_skin = std::move(skin);
}

}  // namespace bmj
