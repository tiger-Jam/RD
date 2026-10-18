// BMjoy：譜面を1つ指定して、LR2スキン（または仮の画面）で遊ぶ
//
//   bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]
//                        [--hs 100] [--seed N] [--timing ms] [--lr2-root DIR] [--skin FILE]
//                        [--skin-opt 番号=選択] [--skin-res sd|hd|fhd] [--no-skin] [--simulate] [--info]
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/mixer.h"
#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "game/judge.h"
#include "input/input.h"
#include "input/keyconfig.h"
#include "render/keyconfig_scene.h"
#include "render/play_scene.h"
#include "render/skin_play.h"
#include "skin/lr2_skin.h"
#include "skin/skin_renderer.h"
#include "util/path.h"

using namespace bmj;
namespace fs = std::filesystem;

namespace {

struct Options {
    std::string chart_path;
    bool autoplay = false;
    int gauge = kGroove;
    bool gas = false;
    int hi_speed = 100;
    uint32_t seed = 0;
    int judge_timing = 0;  // LR2の「判定タイミング」(-199..199ms)。LR2と同じく描画側だけをずらす
    bool simulate = false;
    bool info = false;
    std::string lr2_root;
    std::string skin;
    std::vector<std::pair<int, int>> skin_opts;
    std::string skin_res;
    bool no_skin = false;
    bool keyconfig = false;
    std::string capture_dir;  // 開発用：描画結果を2秒ごとにBMPで保存する
    double quit_after = 0;    // 開発用：指定秒数で終了する
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--auto") o.autoplay = true;
        else if (a == "--gas") o.gas = true;
        else if (a == "--simulate") o.simulate = true;
        else if (a == "--info") o.info = true;
        else if (a == "--no-skin") o.no_skin = true;
        else if (a == "--keyconfig") o.keyconfig = true;
        else if (a == "--capture") o.capture_dir = next();
        else if (a == "--quit-after") o.quit_after = std::atof(next());
        else if (a == "--lr2-root") o.lr2_root = next();
        else if (a == "--skin") o.skin = next();
        else if (a == "--skin-res") o.skin_res = next();
        else if (a == "--skin-opt") {
            std::string v = next();
            size_t eq = v.find('=');
            if (eq == std::string::npos) return false;
            o.skin_opts.push_back({std::atoi(v.substr(0, eq).c_str()), std::atoi(v.substr(eq + 1).c_str())});
        } else if (a == "--hs") o.hi_speed = std::clamp(std::atoi(next()), 10, 900);
        else if (a == "--seed") o.seed = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--timing") o.judge_timing = std::clamp(std::atoi(next()), -199, 199);
        else if (a == "--gauge") {
            std::string g = next();
            if (g == "groove" || g == "normal") o.gauge = kGroove;
            else if (g == "hard") o.gauge = kHard;
            else if (g == "death") o.gauge = kDeath;
            else if (g == "easy") o.gauge = kEasy;
            else if (g == "pattack") o.gauge = kPAttack;
            else if (g == "gattack") o.gauge = kGAttack;
            else {
                std::fprintf(stderr, "不明なゲージ: %s\n", g.c_str());
                return false;
            }
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "不明なオプション: %s\n", a.c_str());
            return false;
        } else {
            o.chart_path = a;
        }
    }
    return o.keyconfig || !o.chart_path.empty();
}

// キー音のファイル名を解決する（LR2: FindAltSound。書かれた名前→拡張子をogg/wavに替える→1つ上のフォルダ）
std::vector<std::string> resolve_sounds(const Chart& chart, std::vector<std::string>& missing) {
    DirIndex dir(chart.base_dir);
    std::vector<std::string> paths(chart.wav.size());
    for (size_t i = 0; i < chart.wav.size(); ++i) {
        if (chart.wav[i].empty()) continue;
        std::string p = dir.resolve(chart.wav[i], {".ogg", ".wav"});
        if (p.empty()) {
            std::string up = join_path(chart.base_dir, "../" + chart.wav[i]);
            if (std::FILE* f = std::fopen(up.c_str(), "rb")) {
                std::fclose(f);
                p = up;
            }
        }
        if (p.empty()) missing.push_back(chart.wav[i]);
        paths[i] = p;
    }
    return paths;
}

void print_result(const Chart& chart, const JudgeEngine& j) {
    const PlayStats& s = j.stats();
    static const char* lamps[] = {"NO PLAY", "FAILED", "EASY", "NORMAL", "HARD", "FULL COMBO"};
    std::printf("結果: %s\n", chart.meta.title.c_str());
    std::printf("  ランプ %s / EX %d / MAX COMBO %d / BP %d\n", lamps[j.failed() ? 1 : j.clear()], s.exscore(),
                s.max_combo, s.bp());
    std::printf("  PG %d GR %d GD %d BD %d PR %d (空POOR %d) FAST %d SLOW %d / %d ノーツ\n", s.judge_count[kPGreat],
                s.judge_count[kGreat], s.judge_count[kGood], s.judge_count[kBad], s.judge_count[kPoor], s.empty_poor,
                s.fast, s.slow, s.total_notes);
    std::printf("  ゲージ %.1f%%\n", j.gauge().hp(j.gauge_type()));
}

void print_info(const Chart& c, const BmsParseResult& r) {
    int counts[4] = {};
    for (const Note& n : c.notes) counts[static_cast<int>(n.kind)]++;
    std::printf("タイトル: %s / %s\n", c.meta.title.c_str(), c.meta.artist.c_str());
    std::printf("BPM %.2f（主BPM %.2f） #RANK %d TOTAL %.0f\n", c.initial_bpm, c.main_bpm, c.judge_rank, c.total);
    std::printf("ノーツ %d（通常+LN）、LN %d、地雷 %d、不可視 %d、BGM %zu、長さ %.2f秒、%s\n", c.judgeable_notes(),
                counts[1], counts[2], counts[3], c.bgm.size(), c.end_us / 1e6,
                c.layout == KeyLayout::K7S ? "7K+皿" : "7K（皿なし）");
    for (int v : r.random_values) std::printf("#RANDOM → %d\n", v);
    for (const auto& w : r.warnings) std::printf("警告: %s\n", w.c_str());
}

// 画面も音も出さずに自動演奏で最後まで判定を回す（動作確認用）
int simulate(const Chart& chart, const Options& o) {
    JudgeConfig cfg;
    cfg.gauge_type = o.gauge;
    cfg.gas = o.gas;
    cfg.autoplay = true;
    JudgeEngine j(chart, cfg);
    for (int64_t t = -1000000; t <= chart.end_us; t += 1000) j.advance(t);
    print_result(chart, j);
    return 0;
}

// LR2のフォルダ（LR2files があるところ）を探す
std::string find_lr2_root(const Options& o, const char* argv0) {
    std::vector<std::string> cands;
    if (!o.lr2_root.empty()) cands.push_back(o.lr2_root);
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

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "使い方: bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]\n"
                     "                [--hs 100] [--seed N] [--timing ms] [--lr2-root DIR] [--skin FILE]\n"
                     "                [--skin-opt 番号=選択] [--skin-res sd|hd|fhd] [--no-skin] [--simulate] [--info]\n"
                     "       bmjoy --keyconfig\n");
        return 2;
    }

    if (opt.keyconfig) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)) return 1;
        SDL_Window* w = SDL_CreateWindow("BMjoy KEY CONFIG", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        SDL_Renderer* r = w ? SDL_CreateRenderer(w, nullptr) : nullptr;
        if (!r) return 1;
        SDL_SetRenderVSync(r, 1);
        std::string path = keyconfig_path();
        KeyConfig kc = KeyConfig::lr2_default();
        kc.load(path);
        run_keyconfig(w, r, kc, path);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(w);
        SDL_Quit();
        return 0;
    }

    BmsParseOptions popt;
    popt.random_seed = opt.seed ? opt.seed : static_cast<uint32_t>(SDL_GetTicksNS() % 0x7fff);
    BmsParseResult parsed = parse_bms_file(opt.chart_path, popt);
    if (!parsed.ok) {
        std::fprintf(stderr, "読み込めない: %s\n", parsed.error.c_str());
        return 1;
    }
    const Chart& chart = parsed.chart;
    if (opt.info) {
        print_info(chart, parsed);
        return 0;
    }
    if (opt.simulate) return simulate(chart, opt);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)) {
        std::fprintf(stderr, "SDLの初期化に失敗: %s\n", SDL_GetError());
        return 1;
    }
    KeyConfig keycfg = KeyConfig::lr2_default();
    if (keycfg.load(keyconfig_path())) std::printf("キーコンフィグ: %s\n", keyconfig_path().c_str());

    // スキン
    PlayContext ctx;
    ctx.chart = &chart;
    ctx.autoplay = opt.autoplay;
    ctx.gauge_type = opt.gauge;
    std::unique_ptr<Lr2Skin> skin;
    if (!opt.no_skin) {
        std::string root = find_lr2_root(opt, argv[0]);
        std::string skin_path = opt.skin.empty() ? resolve_lr2_path(root, "LR2files/Theme/LR2/Play/play_7.lr2skin", false)
                                                 : opt.skin;
        SkinHeader header;
        if (root.empty() || skin_path.empty() || !read_skin_header(root, skin_path, header)) {
            std::printf("LR2スキンが見つからないので、仮の画面で遊ぶ（--lr2-root でLR2のフォルダを指定できる）\n");
        } else {
            for (auto [idx, choice] : opt.skin_opts)
                if (idx >= 0 && idx < static_cast<int>(header.customs.size())) header.customs[idx].selected = choice;
            std::printf("スキン: %s（%s）\n", header.title.c_str(), skin_path.c_str());
            for (size_t i = 0; i < header.customs.size(); ++i) {
                const SkinCustomItem& c = header.customs[i];
                std::string sel = c.selected < static_cast<int>(c.labels.size()) ? c.labels[c.selected] : "?";
                std::printf("  [%zu] %s = %s\n", i, c.title.c_str(), sel.c_str());
            }
            skin = std::make_unique<Lr2Skin>();
            std::string err;
            if (!load_lr2_skin(root, skin_path, header, ctx.load_options(), *skin, err)) {
                std::printf("スキンを読めない: %s\n", err.c_str());
                skin.reset();
            } else {
                if (!header.has_resolution) guess_resolution(*skin);
                if (opt.skin_res == "sd") skin->width = 640, skin->height = 480;
                else if (opt.skin_res == "hd") skin->width = 1280, skin->height = 720;
                else if (opt.skin_res == "fhd") skin->width = 1920, skin->height = 1080;
                for (const auto& w : skin->warnings) std::printf("  スキン警告: %s\n", w.c_str());
                ctx.skin = skin.get();
            }
        }
    }
    int view_w = skin ? skin->width : 640, view_h = skin ? skin->height : 480;
    int win_w = view_w <= 640 ? 1280 : view_w, win_h = view_w <= 640 ? 960 : view_h;
    SDL_Window* window = SDL_CreateWindow("BMjoy", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "ウィンドウを作れない: %s\n", SDL_GetError());
        return 1;
    }
    // スキン解像度で描いて拡大する（LR2と同じ見た目）
    SDL_SetRenderLogicalPresentation(renderer, view_w, view_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);
    SkinRenderer skin_renderer(renderer);
    if (skin) skin_renderer.upload(*skin);

    Mixer mixer;
    if (!mixer.open(0, 128)) {
        std::fprintf(stderr, "音声デバイスを開けない\n");
        return 1;
    }
    std::printf("音声: %u Hz、出力遅延の推定 %.1f ms\n", mixer.sample_rate(), mixer.output_latency_us() / 1000.0);

    // キー音はワーカーで読み込む。読み込み中もスキンの読み込み画面を動かす
    std::vector<std::string> missing;
    std::vector<std::string> paths = resolve_sounds(chart, missing);
    for (const auto& m : missing) std::printf("キー音が見つからない: %s\n", m.c_str());
    int total_sounds = static_cast<int>(std::count_if(paths.begin(), paths.end(), [](auto& p) { return !p.empty(); }));
    SoundBank bank;
    std::vector<std::string> load_errors;
    std::atomic<bool> loaded{false};
    std::atomic<int> load_done{0};
    std::thread loader([&] {
        int threads = std::max(2u, std::thread::hardware_concurrency() / 2) + 1;
        bank.load(paths, mixer.sample_rate(), threads, &load_errors, &load_done);
        loaded = true;
    });
    bool loader_joined = false;
    ctx.load_total = std::max(1, total_sounds);

    bool quit = false, retry = true;
    while (retry && !quit) {
        retry = false;
        JudgeConfig jcfg;
        jcfg.gauge_type = opt.gauge;
        jcfg.gas = opt.gas;
        jcfg.autoplay = opt.autoplay;
        JudgeEngine judge(chart, jcfg);
        ctx.judge = &judge;
        ctx.timers = SkinTimers();
        ctx.song_ms = -1;
        ctx.judge_draw = 0;
        ctx.last_miss_ms = -1e9;
        ctx.hi_speed = opt.hi_speed;

        InputMapper input(keycfg);
        PlayViewState view;
        view.hi_speed = opt.hi_speed;
        view.autoplay = opt.autoplay;
        std::vector<LaneInput> lane_in;
        std::vector<ButtonInput> button_in;
        bool fullscreen = false;
        bool started = false;
        enum class Phase { Play, Fadeout, Failed, Result } phase = Phase::Play;
        const uint64_t scene_start = SDL_GetTicksNS();
        uint64_t prev_ns = scene_start;
        int next_capture_s = 0;
        float scratch_angle = 0;
        int old_hp = -1;
        bool last_note_set = false, fc_set = false;
        int fps_frames = 0;
        uint64_t fps_t0 = scene_start;
        ctx.timers.set(0, 0);

        while (!quit && !retry) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_EVENT_QUIT: quit = true; break;
                case SDL_EVENT_JOYSTICK_ADDED: SDL_OpenJoystick(e.jdevice.which); break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.repeat) break;
                    if (e.key.key == SDLK_ESCAPE) {
                        if (phase == Phase::Result) quit = true;
                        else phase = Phase::Result, mixer.stop(), print_result(chart, judge);
                    } else if (e.key.key == SDLK_RETURN && phase == Phase::Result) {
                        quit = true;
                    } else if (e.key.key == SDLK_F5 && phase == Phase::Result) {
                        retry = true;
                    } else if (e.key.key == SDLK_F4) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(window, fullscreen);
                    } else if (e.key.key == SDLK_UP) {
                        view.hi_speed = std::min(900, view.hi_speed + 10);
                    } else if (e.key.key == SDLK_DOWN) {
                        view.hi_speed = std::max(10, view.hi_speed - 10);
                    } else if (e.key.key == SDLK_RIGHT) {
                        opt.judge_timing = std::min(199, opt.judge_timing + 1);
                    } else if (e.key.key == SDLK_LEFT) {
                        opt.judge_timing = std::max(-199, opt.judge_timing - 1);
                    }
                    break;
                default: break;
                }
                if (phase == Phase::Play) input.handle(e);
            }

            uint64_t now_ns = SDL_GetTicksNS();
            double now_ms = (now_ns - scene_start) / 1e6;
            double dt_ms = (now_ns - prev_ns) / 1e6;
            prev_ns = now_ns;
            ctx.now_ms = now_ms;
            ctx.hi_speed = view.hi_speed;
            ctx.judge_timing = opt.judge_timing;
            ctx.loaded = load_done.load();
            if (++fps_frames, now_ns - fps_t0 > 1000000000ull) {
                ctx.fps = static_cast<int>(fps_frames * 1e9 / (now_ns - fps_t0) + 0.5);
                fps_frames = 0;
                fps_t0 = now_ns;
            }
            if (loaded && !loader_joined) {
                loader.join();
                loader_joined = true;
                for (const auto& er : load_errors) std::printf("%s\n", er.c_str());
            }

            // 場面の進行（LR2: Scene04_Play.cpp:1748-1768）
            int startinput = skin ? skin->startinput : 0;
            int load_wait = skin ? skin->loadstart + skin->loadend : 0;
            int playstart = skin ? skin->playstart : 1000;
            if (!ctx.timers.on(1) && now_ms >= startinput) ctx.timers.set(1, now_ms);
            if (!ctx.timers.on(40) && loader_joined && now_ms >= load_wait) ctx.timers.set(40, now_ms);
            if (ctx.timers.on(40) && !started && ctx.timers.lapse(40, now_ms) >= playstart) {
                std::vector<ScheduledSound> schedule;
                uint32_t rate = mixer.sample_rate();
                auto frame_of = [&](int64_t us) { return us * static_cast<int64_t>(rate) / 1000000; };
                for (const BgmEvent& b : chart.bgm) schedule.push_back({frame_of(b.time_us), b.sound});
                if (opt.autoplay)
                    for (const Note& n : chart.notes)
                        if ((n.kind == NoteKind::Normal || n.kind == NoteKind::Long) && n.sound)
                            schedule.push_back({frame_of(n.time_us), n.sound});
                mixer.set_song(&bank.samples(), std::move(schedule));
                mixer.start(0);
                started = true;
            }

            int64_t song_us = started ? mixer.song_time_us_at(now_ns) : 0;
            double song_ms = song_us / 1000.0;
            // 曲の時刻 → シーンの時刻（タイマーは曲の出来事の瞬間に合わせて立てる）
            auto scene_ms_of = [&](int64_t t_us) { return now_ms - (song_ms - t_us / 1000.0); };

            if (started && phase == Phase::Play) {
                ctx.song_ms = song_ms;
                ctx.timers.set(41, now_ms - song_ms);
                ctx.timers.set(142, now_ms - song_ms);
                lane_in.clear();
                button_in.clear();
                input.collect(now_ns, lane_in, button_in);
                for (const LaneInput& li : lane_in) {
                    int64_t t = mixer.song_time_us_at(li.time_ns);
                    if (li.down) judge.press(li.lane, t);
                    else judge.release(li.lane, t);
                    // 鍵盤の光（LR2: LR2_gameloop.cpp:120）
                    double at = now_ms - (now_ns - li.time_ns) / 1e6;
                    if (li.down) ctx.timers.set(100 + li.lane, at), ctx.timers.reset(120 + li.lane);
                    else ctx.timers.reset(100 + li.lane), ctx.timers.set(120 + li.lane, at);
                }
                // 入力が遅れて届く分の余裕を見て、少し手前の時刻まで進める
                judge.advance(song_us - 5000);
                for (uint16_t s : judge.sounds_to_stop()) mixer.stop_now(s);
                for (uint16_t s : judge.sounds_to_play()) mixer.play_now(s);
                judge.sounds_to_stop().clear();
                judge.sounds_to_play().clear();
                for (const JudgeEvent& je : judge.events()) {
                    double at = scene_ms_of(je.time_us);
                    int j = je.judge >= 100 ? je.judge - 100 : je.judge;
                    if (je.judge >= 100) {
                        // LNの始点（LR2: ProcLongnote。PG/GR/GDでタイマー70）
                        if (j >= kGood) ctx.timers.set(70 + je.lane, at);
                    } else {
                        if (j >= kGreat) ctx.timers.set(50 + je.lane, at);  // ボム
                        if (j < kGood) ctx.last_miss_ms = at;
                        ctx.judge_draw = j;
                        ctx.timers.set(46, at);
                    }
                    view.last_judge = j;
                    view.last_offset_ms = je.offset_ms;
                    view.last_fast = je.fast;
                    view.last_has_offset = je.has_offset;
                    view.last_judge_ms = je.time_us / 1000.0;
                    if (opt.autoplay && je.judge < 100) {
                        ctx.timers.set(100 + je.lane, at);
                        ctx.timers.reset(120 + je.lane);
                    }
                }
                judge.events().clear();
                for (int lane = 0; lane < kLaneCount; ++lane) {
                    if (!judge.lane_ln_holding(lane)) ctx.timers.reset(70 + lane);
                    // 自動演奏の鍵盤の光は80msで消す（LR2: Play.cpp:684）
                    if (opt.autoplay && ctx.timers.lapse(50 + lane, now_ms) >= 80 && ctx.timers.lapse(100 + lane, now_ms) > 0) {
                        ctx.timers.reset(100 + lane);
                        ctx.timers.set(120 + lane, now_ms);
                    }
                }
                view.combo = judge.stats().combo;
                const PlayStats& ps = judge.stats();
                if (!last_note_set && ps.note_current >= ps.total_notes) ctx.timers.set(143, now_ms), last_note_set = true;
                if (!fc_set && ps.total_notes > 0 && ps.combo == ps.total_notes) ctx.timers.set(48, now_ms), fc_set = true;
                // ゲージのタイマー（LR2: ApplyJudgeNote の 42/44）
                int hp = static_cast<int>(judge.gauge().hp(judge.gauge_type())) / 2 * 2;
                if (old_hp >= 0 && hp != old_hp) {
                    if (old_hp <= 0) hp = 0, ctx.timers.reset(44);
                    else if (hp != 100) ctx.timers.reset(44);
                    if (old_hp < hp) {
                        if (hp == 100) ctx.timers.reset(42), ctx.timers.set(44, now_ms);
                        else ctx.timers.set(42, now_ms);
                    }
                }
                old_hp = hp;

                bool survival = !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                if (survival && judge.failed()) {
                    phase = Phase::Failed;
                    ctx.timers.set(3, now_ms);
                    mixer.stop();
                    view.failed = true;
                } else if (song_us > chart.end_us) {
                    phase = Phase::Fadeout;
                    ctx.timers.set(2, now_ms);
                }
            }
            if (phase == Phase::Fadeout && ctx.timers.lapse(2, now_ms) >= (skin ? skin->fadeout : 0)) {
                phase = Phase::Result;
                mixer.stop();
                print_result(chart, judge);
            } else if (phase == Phase::Failed && ctx.timers.lapse(3, now_ms) >= (skin ? skin->close : 2050)) {
                phase = Phase::Result;
                print_result(chart, judge);
            }

            // 皿の回転（LR2: LR2_gameloop.cpp:349。押している向きに速く、離していればゆっくり回る）
            if (input.button_held(kScratchA)) scratch_angle -= static_cast<float>(dt_ms * 0.72);
            else if (input.button_held(kScratchB)) scratch_angle += static_cast<float>(dt_ms * 0.72);
            else scratch_angle += static_cast<float>(dt_ms * 0.36);
            scratch_angle = std::fmod(scratch_angle, 360.0f);

            double render_beat = chart.pos_at(song_us + opt.judge_timing * 1000);
            if (phase == Phase::Result) {
                SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
                draw_result(renderer, chart, judge);
            } else if (skin) {
                // タイマー140：小節線からの拍数×1000
                ctx.timers.rhythm = -1;
                if (started)
                    for (const BarLine& b : chart.bars)
                        if (b.pos <= render_beat) ctx.timers.rhythm = (render_beat - b.pos) * 1000.0;
                PlaySkinState st;
                st.now_ms = now_ms;
                st.timers = &ctx.timers;
                st.option = [&](int o) { return ctx.option(o); };
                st.number = [&](int id) { return ctx.number(id); };
                st.bargraph = [&](int t, double& v, double& m) { return ctx.bargraph(t, v, m); };
                st.slider = [&](int t, int& v, int& lo, int& hi) { return ctx.slider(t, v, lo, hi); };
                st.text = [&](int id) { return ctx.text(id); };
                st.chart = &chart;
                st.judge = &judge;
                st.notes_visible = started;
                st.render_beat = render_beat;
                st.hi_speed = view.hi_speed;
                st.autoplay = opt.autoplay;
                st.judge_draw = ctx.judge_draw;
                st.combo_draw = judge.stats().combo;
                st.gauge_hp = judge.gauge().hp(judge.gauge_type());
                st.gauge_survival = !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                st.scratch_angle = scratch_angle;
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                skin_renderer.draw_play(*skin, st);
            } else if (!started) {
                draw_loading(renderer, chart, load_done.load(), total_sounds);
            } else {
                view.song_ms = song_ms;
                view.render_beat = render_beat;
                char status[128];
                std::snprintf(status, sizeof(status), "UP/DOWN: HI-SPEED  LEFT/RIGHT: TIMING %+d ms  F4: FULLSCREEN  ESC: END",
                              opt.judge_timing);
                view.status = status;
                draw_play(renderer, chart, judge, view);
            }

            if (!opt.capture_dir.empty() && (now_ms / 1000 >= next_capture_s * 2 || phase == Phase::Result)) {
                if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "/frame_%03d%s.bmp", next_capture_s * 2,
                                  phase == Phase::Result ? "_result" : "");
                    SDL_SaveBMP(shot, (opt.capture_dir + name).c_str());
                    SDL_DestroySurface(shot);
                }
                if (phase == Phase::Result) opt.capture_dir.clear();
                ++next_capture_s;
            }
            SDL_RenderPresent(renderer);
            if (opt.quit_after > 0 && now_ms / 1000 > opt.quit_after) quit = true;
        }
        mixer.stop();
        if (skin) SDL_SetRenderLogicalPresentation(renderer, view_w, view_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
    if (!loader_joined) loader.join();

    mixer.close();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
