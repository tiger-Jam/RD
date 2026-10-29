#include "app/app.h"

#include <algorithm>
#include <cstdio>

#include "app/bgm_set.h"
#include "app/skin_catalog.h"
#include "skin/skin_compose.h"
#include "audio/system_sound.h"
#include "render/frame_hooks.h"
#include "util/data_dir.h"
#include "util/dev.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace rd {

// LR2のフォルダ（LR2files があるところ）を探す
std::string find_lr2_root(const std::string& configured, const char* argv0) {
    std::vector<std::string> cands;
    if (!configured.empty()) cands.push_back(configured);
    if (const char* env = dev_env("RD_LR2_ROOT")) cands.push_back(env);
    // 実行ファイルの横（LR2 と同じく、RD のフォルダに LR2files を置いて自己完結させる）
    if (const char* base = SDL_GetBasePath()) cands.push_back(base);
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
// 見つからなければSD（RDの拡張。OpenLR2は設定の既定値を使う）
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
    window = SDL_CreateWindow("Revenant Deliria", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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
    apply_volume();
    apply_sound_fx();
    load_mods();
    load_sound_set();
    frame_hooks_install([this] { mixer.play_system(kSysScreenshot); });
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

// システム音（LR2のサウンドセット）。スキンセレクトで替えたときも読み直す
void App::load_sound_set() {
    std::string ss = cfg.sound_set.empty() ? resolve_lr2_path(lr2_root, "LR2files/Sound/lr2.lr2ss", false) : cfg.sound_set;
    if (lr2_root.empty() || ss.empty()) return;
    // サウンドセットのカスタマイズ（#CUSTOMFILE で選んだ BGM のフォルダなど）。RANDOM なら候補から 1 つ選ぶ
    std::vector<std::string> chosen;
    SkinHeader header;
    if (read_skin_header(lr2_root, ss, header)) {
        apply_skin_custom(cfg, ss, header);
        for (const SkinCustomItem& it : header.customs) {
            if (!it.is_file) continue;
            std::string name = it.selected >= 0 && it.selected < static_cast<int>(it.labels.size()) ? it.labels[it.selected] : "";
            if (name == "RANDOM") {
                std::vector<std::string> cands;
                for (const std::string& l : it.labels)
                    if (l != "RANDOM") cands.push_back(l);
                name = cands.empty() ? std::string() : cands[SDL_GetTicksNS() % cands.size()];
            }
            chosen.push_back(name);
        }
    }
    std::vector<Sample> sounds = load_system_sounds(lr2_root, ss, mixer.sample_rate(), chosen);
    // キー音を 16 ビットで持つ設定なら、効果音と選曲の BGM も 16 ビットで持つ
    if (cfg.sound_16bit)
        for (Sample& smp : sounds) smp.to_16bit();
    // KEY 側のシステム音（パネル・フォルダ・オプション・難易度・スクリーンショット・地雷・皿）。それ以外は BGM 側（LR2 と同じ）
    uint32_t key_side = 0;
    for (int id : {kSysFolderOpen, kSysFolderClose, kSysPanelOpen, kSysPanelClose, kSysOptionChange, kSysDifficulty, kSysScreenshot,
                   kSysMine, kSysScratch})
        key_side |= 1u << id;
    mixer.set_system_key_side(key_side);
    int n = 0;
    for (const Sample& smp : sounds) n += !smp.empty();
    std::printf("システム音: %d / %zu（%s）\n", n, sounds.size(), ss.c_str());
    mixer.set_system_sounds(std::move(sounds));
}

// ウィンドウの大きさ。解像度（SD/HD/FHD）の指定があればその大きさ、無ければスキンの大きさ（SD は 2 倍）。
// スキンは自分の大きさで描いて、ウィンドウいっぱいに拡大する（縦横比は保つ）
void App::window_size(int skin_w, int skin_h, int& w, int& h) const {
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

void App::load_mods() {
    std::vector<std::string> errors;
    mods = scan_mods(official_mods_dir(), true, &errors);  // 公式の MOD だけを読む（自分で入れる MOD は、今は読まない）
    // 起動の設定画面で変えた詳細の設定（GAS の順番、カスタムゲージ）を重ねる
    for (ModManifest& m : mods) apply_mod_settings(m, mod_settings_path(m.id));
    apply_lamp_colors(mods, cfg.enabled_mods);  // EX-HARD などのランプの色を、カスタムゲージ MOD で変えていれば
    resolve_gauge_lamps(mods);
    for (const std::string& e : errors) std::printf("MOD を読めない：%s\n", e.c_str());
    for (const ModManifest* m : active_mods()) std::printf("MOD：%s %s\n", m->name.c_str(), m->version.c_str());
}

std::vector<const ModManifest*> App::active_mods() const {
    std::vector<const ModManifest*> out;
    for (const std::string& id : cfg.enabled_mods)
        for (const ModManifest& m : mods)
            if (m.id == id) {
                out.push_back(&m);
                break;
            }
    return out;
}

std::vector<std::pair<std::string, const CustomGaugeDef*>> App::custom_gauges() const {
    std::vector<std::pair<std::string, const CustomGaugeDef*>> out;
    for (const ModManifest* m : active_mods())
        for (const CustomGaugeDef& g : m->gauges) out.push_back({m->id + "/" + g.id, &g});
    return out;
}

std::map<std::string, const CustomLampDef*> App::custom_lamps() const {
    std::map<std::string, const CustomLampDef*> out;
    for (const ModManifest* m : active_mods())
        for (const CustomLampDef& l : m->lamps) out[m->id + "/" + l.id] = &l;
    return out;
}

uint32_t App::standard_lamp_tint(int lamp, double ms) const {
    if (lamp < 1 || lamp > 6) return 0;
    for (const ModManifest* m : active_mods()) {
        const CustomLampDef& l = m->standard_lamps[lamp - 1];
        if (l.colors > 0) return 0xff000000u | l.color_at(ms);
    }
    return 0;
}

const GaugeColors* App::standard_gauge_colors(int gauge_type) const {
    if (gauge_type < 0 || gauge_type >= 6) return nullptr;
    for (const ModManifest* m : active_mods())
        if (m->standard_gauges[gauge_type].colors > 0) return &m->standard_gauges[gauge_type];
    return nullptr;
}

void App::cycle_gauge(int dir) {
    // 並び：GROOVE … G-ATTACK（LR2 の番号の順）→ カスタムゲージ → GAS（GAS の MOD を使っていれば）
    auto gauges = custom_gauges();
    std::vector<std::string> keys;
    for (const auto& g : gauges) keys.push_back(g.first);
    if (!gas_order().empty()) keys.push_back(kGasKey);
    const int n = kGaugeCount + static_cast<int>(keys.size());
    int cur = cfg.gauge;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == cfg.custom_gauge) cur = kGaugeCount + static_cast<int>(i);
    cur = ((cur + dir) % n + n) % n;
    if (cur < kGaugeCount) cfg.gauge = cur, cfg.custom_gauge.clear();
    else cfg.custom_gauge = keys[cur - kGaugeCount];
}

std::string App::custom_gauge_name() const {
    if (cfg.custom_gauge == kGasKey && !gas_order().empty()) return "GAS";
    for (const auto& [key, g] : custom_gauges())
        if (key == cfg.custom_gauge) return g->name;
    return {};
}

// 同じスキンのファイルか（区切りの向き・「..」・大文字小文字の違いを無視する。スキンの一覧の見分け方と同じ）
static bool same_skin_path(const std::string& a, const std::string& b) {
    auto norm = [](const std::string& p) {
        std::error_code ec;
        fs::path q = fs::weakly_canonical(fs::path(p), ec);
        std::string s = (ec ? fs::path(p) : q).lexically_normal().generic_string();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return norm(a) == norm(b);
}

void App::apply_skin_mods(Lr2Skin& sk, const std::string& path, const std::array<bool, 1000>& options, PartSkinCache* part_cache) const {
    // 色は、部品が元々どのスキンのものかで決める（スキンを決めていないものは全部）。
    // 付け替える前に、遊ぶスキンと、部品を持ってくるスキンのそれぞれに当てる
    const std::vector<SkinColorFilter> all = skin_colors();
    int colored = 0;
    auto colors_for = [&](Lr2Skin& target, const std::string& skin_path) {
        std::vector<SkinColorFilter> use;
        for (const SkinColorFilter& f : all)
            if (f.skins.empty() ||
                std::any_of(f.skins.begin(), f.skins.end(), [&](const std::string& s) { return same_skin_path(from_portable(s), skin_path); }))
                use.push_back(f);
        colored += apply_skin_colors(target, use);
    };
    colors_for(sk, path);
    // ほかのスキンから持ってくる部品（スキン選択で選んだもの）。カスタマイズはそのスキンの設定に、部品ごとに選んだ絵の種類を重ねる。
    // 同じスキンを同じカスタマイズで使う部品どうしは、1 回だけ読む
    if (auto it = cfg.skin_parts.find(path); it != cfg.skin_parts.end()) {
        std::map<std::string, std::shared_ptr<const Lr2Skin>> others;
        int n = 0;
        for (const PlayPartSlot& slot : kPlayPartSlots) {
            auto f = it->second.from.find(slot.key);
            if (f == it->second.from.end()) continue;
            static const std::map<int, std::string> none;
            auto c = it->second.custom.find(slot.key);
            const std::map<int, std::string>& custom = c != it->second.custom.end() ? c->second : none;
            std::string key = f->second;
            for (const auto& [i, name] : custom) key += "|" + std::to_string(i) + "=" + name;
            auto [o, fresh] = others.try_emplace(key);
            // 覚えておく鍵：スキン自身のカスタマイズと、読み込みの条件も含める
            std::string cache_key;
            if (fresh && part_cache) {
                cache_key = key;
                if (auto sc = cfg.skin_custom.find(f->second); sc != cfg.skin_custom.end()) cache_key += "#" + format_skin_custom(sc->second);
                cache_key += "#";
                for (bool b : options) cache_key += b ? '1' : '0';
                if (auto hit = part_cache->find(cache_key); hit != part_cache->end()) o->second = hit->second, fresh = false;
            }
            if (fresh) {
                SkinHeader h;
                std::string err;
                auto skin = std::make_shared<Lr2Skin>();
                if (read_skin_header(lr2_root, f->second, h)) {
                    apply_skin_custom(cfg, f->second, h);
                    for (const auto& [i, name] : custom)
                        if (i >= 0 && i < static_cast<int>(h.customs.size()) && h.customs[i].is_file)
                            for (size_t j = 0; j < h.customs[i].labels.size(); ++j)
                                if (h.customs[i].labels[j] == name) h.customs[i].selected = static_cast<int>(j);
                    if (load_lr2_skin(lr2_root, f->second, h, options, *skin, err)) {
                        if (!h.has_resolution) guess_resolution(*skin);
                        colors_for(*skin, f->second);
                        o->second = std::move(skin);
                        if (part_cache) {
                            if (part_cache->size() >= 6) part_cache->clear();  // 画素ごと持つので、増えすぎたら捨てる
                            (*part_cache)[cache_key] = o->second;
                        }
                    }
                }
                if (!o->second) std::printf("  部品を持ってくるスキンを読めない: %s\n", f->second.c_str());
            }
            if (o->second)
                n += compose_play_part(sk, *o->second, slot.key, it->second.screen_scale ? PartScale::Screen : PartScale::Lane);
        }
        if (n) std::printf("  ほかのスキンから持ってきた部品: %d\n", n);
    }
    if (colored) std::printf("  部品の色を変えた: %d\n", colored);
}

void App::pick_bgm() {
    // BGM のセット（MOD）で使うセットがあれば、選曲・決定の BGM をそれぞれの候補からランダムに 1 つずつ選んで替える
    for (const ModManifest* m : active_mods()) {
        if (!m->has_setting("bgm") || m->bgm_use.empty()) continue;
        for (const ModManifest::BgmSet& set : m->bgm_sets) {
            if (set.name != m->bgm_use) continue;
            for (const auto& [list, id] : {std::pair<const std::vector<std::string>*, int>{&set.select, kSysSelect}, {&set.decide, kSysDecide}}) {
                if (list->empty()) continue;
                const std::string& key = (*list)[std::uniform_int_distribution<size_t>(0, list->size() - 1)(bgm_rng_)];
                Sample smp;
                if (load_bgm_choice(lr2_root, cfg, key, id, mixer.sample_rate(), smp)) {
                    if (cfg.sound_16bit) smp.to_16bit();
                    mixer.set_system_sound(id, std::move(smp));
                    std::printf("BGM のセット %s：%s ← %s\n", set.name.c_str(), id == kSysSelect ? "選曲" : "決定", key.c_str());
                } else {
                    std::printf("BGM のセットの音を読めない: %s\n", key.c_str());
                }
            }
            return;
        }
    }
}

std::vector<SkinColorFilter> App::skin_colors() const {
    std::vector<SkinColorFilter> out;
    for (const ModManifest* m : active_mods()) out.insert(out.end(), m->skin_colors.begin(), m->skin_colors.end());
    return out;
}

const ModManifest* App::metronome() const {
    for (const ModManifest* m : active_mods())
        if (m->metronome) return m;
    return nullptr;
}

std::vector<std::string> App::gas_order() const {
    for (const ModManifest* m : active_mods())
        if (m->gas) return m->gas_order;
    return {};
}

int App::mod_profile(bool seven_key) {
    const std::string key = play_mod_key(active_mods());
    return db.mod_profile(seven_key ? seven_key_record_key(key) : key);
}

void App::toggle_fullscreen() {
    cfg.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    SDL_SetWindowFullscreen(window, cfg.fullscreen);
    save_config();
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
    frame_target_set(renderer, w, h);
}

namespace {

// .lr2skin を読む。configured が空なら LR2 の既定のスキン（default_rel）を使う
std::unique_ptr<Lr2Skin> load_skin(App& app, const std::string& configured, const char* default_rel,
                                   const std::array<bool, 1000>& options, SkinRenderer& renderer, bool play = false) {
    // 引き継ぎ元は前に読んだスキン。読めなかったときに、置き換えで消えるスキンを指したまま残らないよう、先に外す
    const Lr2Skin* previous = app.last_skin;
    app.last_skin = nullptr;
    std::string skin_path = configured.empty() ? resolve_lr2_path(app.lr2_root, default_rel, false) : configured;
    SkinHeader header;
    if (app.lr2_root.empty() || skin_path.empty() || !read_skin_header(app.lr2_root, skin_path, header)) return nullptr;
    apply_skin_custom(app.cfg, skin_path, header);
    std::printf("スキン: %s（%s）\n", header.title.c_str(), skin_path.c_str());
    for (size_t i = 0; i < header.customs.size(); ++i) {
        const SkinCustomItem& c = header.customs[i];
        std::string sel = c.selected >= 0 && c.selected < static_cast<int>(c.labels.size()) ? c.labels[c.selected]
                          : c.is_file                                                        ? "（既定）" + c.default_value
                                                                                             : "?";
        std::printf("  [%zu] %s = %s\n", i, c.title.c_str(), sel.c_str());
    }
    auto skin = std::make_unique<Lr2Skin>();
    std::string err;
    const uint64_t t0 = SDL_GetTicksNS();
    if (!load_lr2_skin(app.lr2_root, skin_path, header, options, *skin, err, previous)) {
        std::printf("スキンを読めない: %s\n", err.c_str());
        return nullptr;
    }
    skin->path = skin_path;
    if (!header.has_resolution) guess_resolution(*skin);
    // MOD でほかのスキンの部品を持ってくる・部品の色を変える（プレースキンだけ）
    if (play) app.apply_skin_mods(*skin, skin_path, options);
    for (const auto& w : skin->warnings)
        if (w.find(".dummy") == std::string::npos) std::printf("  スキン警告: %s\n", w.c_str());
    const uint64_t t1 = SDL_GetTicksNS();
    renderer.upload(*skin);
    // GPU に送った画像の画素は捨てる（同じ画像を二重に持たない。引き継ぐときや反転するときはファイルから読み直す）
    if (app.cfg.low_memory) release_skin_pixels(*skin);
    app.last_skin = skin.get();
    if (dev_env("RD_TIMING"))
        std::printf("  スキンの読み込み %.0f ms、GPU に送る %.0f ms\n", (t1 - t0) / 1e6, (SDL_GetTicksNS() - t1) / 1e6);
    return skin;
}

}  // namespace

void App::load_play_skin(const std::array<bool, 1000>& options) {
    play_skin = load_skin(*this, cfg.play_skin, "LR2files/Theme/LR2/Play/play_7.lr2skin", options, *skin_renderer, true);
    if (!play_skin) std::printf("LR2スキンが見つからないので、仮の画面で遊ぶ（--lr2-root でLR2のフォルダを指定できる）\n");
}

void App::load_select_skin(const std::array<bool, 1000>& options) {
    if (cfg.select_skin == "none") {
        if (last_skin == select_skin.get()) last_skin = nullptr;
        select_skin.reset();
        return;
    }
    select_skin = load_skin(*this, cfg.select_skin, "LR2files/Theme/LR2/Select/select.lr2skin", options, *select_renderer);
}

void App::load_decide_skin(const std::array<bool, 1000>& options) {
    if (cfg.decide_skin == "none") {
        if (last_skin == decide_skin.get()) last_skin = nullptr;
        decide_skin.reset();
        return;
    }
    decide_skin = load_skin(*this, cfg.decide_skin, "LR2files/Theme/LR2/Decide/decide.lr2skin", options, *decide_renderer);
}

std::unique_ptr<Lr2Skin> App::load_scene_skin(const std::string& configured, const char* default_rel,
                                              const std::array<bool, 1000>& options, SkinRenderer& renderer) {
    if (configured == "none") return nullptr;
    return load_skin(*this, configured, default_rel, options, renderer);
}

void App::load_result_skin(const std::array<bool, 1000>& options) {
    if (last_skin == courseresult_skin.get()) last_skin = nullptr;
    courseresult_skin.reset();
    result_skin = load_skin(*this, cfg.result_skin, "LR2files/Theme/LR2/Result/result.lr2skin", options, *result_renderer);
}

// コースリザルトは、リザルトと同じ描画器を使う（同時には出さないので）
void App::load_courseresult_skin(const std::array<bool, 1000>& options) {
    if (last_skin == result_skin.get()) last_skin = nullptr;
    result_skin.reset();
    courseresult_skin = load_skin(*this, cfg.courseresult_skin, "LR2files/Theme/LR2/CourseResult/courseresult.lr2skin", options,
                                  *result_renderer);
}

}  // namespace rd
