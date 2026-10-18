// BMjoy ロードマップ1段階目：譜面を1つ指定して、仮のプレイ画面で遊ぶ
//
//   bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]
//                        [--hs 100] [--seed N] [--timing ms] [--simulate] [--info]
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/mixer.h"
#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "game/judge.h"
#include "input/input.h"
#include "render/play_scene.h"
#include "util/path.h"

using namespace bmj;

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
        else if (a == "--capture") o.capture_dir = next();
        else if (a == "--quit-after") o.quit_after = std::atof(next());
        else if (a == "--hs") o.hi_speed = std::clamp(std::atoi(next()), 10, 900);
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
    return !o.chart_path.empty();
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

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "使い方: bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]\n"
                     "                [--hs 100] [--seed N] [--timing ms] [--simulate] [--info]\n");
        return 2;
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
    SDL_Window* window = SDL_CreateWindow("BMjoy", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "ウィンドウを作れない: %s\n", SDL_GetError());
        return 1;
    }
    // スキン解像度（640x480）で描いて拡大する（LR2と同じ見た目）
    SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);

    Mixer mixer;
    if (!mixer.open(0, 128)) {
        std::fprintf(stderr, "音声デバイスを開けない\n");
        return 1;
    }
    std::printf("音声: %u Hz、出力遅延の推定 %.1f ms\n", mixer.sample_rate(), mixer.output_latency_us() / 1000.0);

    // キー音をワーカーで読み込みながら、読み込み画面を出す
    std::vector<std::string> missing;
    std::vector<std::string> paths = resolve_sounds(chart, missing);
    for (const auto& m : missing) std::printf("キー音が見つからない: %s\n", m.c_str());
    int total_sounds = static_cast<int>(std::count_if(paths.begin(), paths.end(), [](auto& p) { return !p.empty(); }));
    SoundBank bank;
    std::vector<std::string> load_errors;
    std::atomic<bool> loaded{false};
    std::thread loader([&] {
        int threads = std::max(2u, std::thread::hardware_concurrency() / 2) + 1;
        bank.load(paths, mixer.sample_rate(), threads, &load_errors);
        loaded = true;
    });

    bool quit = false;
    while (!loaded && !quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) quit = true;
        draw_loading(renderer, chart, 0, total_sounds);
        SDL_RenderPresent(renderer);
    }
    loader.join();
    for (const auto& e : load_errors) std::printf("%s\n", e.c_str());
    if (quit) return 0;

    bool retry = true;
    while (retry && !quit) {
        retry = false;
        JudgeConfig jcfg;
        jcfg.gauge_type = opt.gauge;
        jcfg.gas = opt.gas;
        jcfg.autoplay = opt.autoplay;
        JudgeEngine judge(chart, jcfg);

        // BGM（と自動演奏ならキー音）をフレーム単位の予定表にする
        uint32_t rate = mixer.sample_rate();
        std::vector<ScheduledSound> schedule;
        auto frame_of = [&](int64_t us) { return us * static_cast<int64_t>(rate) / 1000000; };
        for (const BgmEvent& b : chart.bgm) schedule.push_back({frame_of(b.time_us), b.sound});
        if (opt.autoplay)
            for (const Note& n : chart.notes)
                if ((n.kind == NoteKind::Normal || n.kind == NoteKind::Long) && n.sound)
                    schedule.push_back({frame_of(n.time_us), n.sound});
        mixer.set_song(&bank.samples(), std::move(schedule));
        // LR2の既定スキンの #PLAYSTART（1000ms）に合わせて、1秒後に譜面の時刻0が来る
        mixer.start(rate);

        InputMapper input;
        PlayViewState view;
        view.hi_speed = opt.hi_speed;
        view.autoplay = opt.autoplay;
        std::vector<LaneInput> lane_in;
        std::vector<ButtonInput> button_in;
        bool fullscreen = false;
        uint64_t started_ns = SDL_GetTicksNS();
        int next_capture_s = 0;
        enum class Phase { Play, Failed, Result } phase = Phase::Play;
        uint64_t phase_start_ns = 0;

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
                        else phase = Phase::Result, mixer.stop();
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
            int64_t song_us = mixer.song_time_us_at(now_ns);
            if (std::getenv("BMJ_DEBUG_CLOCK")) {
                // 開発用：オーディオクロックと実時間の差（ずれが育たないこと、揺れが小さいことを見る）
                static int64_t first_diff = INT64_MIN, prev = INT64_MIN, max_jump = 0;
                int64_t diff = song_us - static_cast<int64_t>((now_ns - started_ns) / 1000);
                if (first_diff == INT64_MIN) first_diff = diff;
                if (prev != INT64_MIN) max_jump = std::max<int64_t>(max_jump, std::llabs(diff - prev));
                prev = diff;
                static int last_report = -1;
                int sec = static_cast<int>((now_ns - started_ns) / 1000000000);
                if (sec % 5 == 0 && sec != last_report) {
                    last_report = sec;
                    std::printf("clock t=%2ds 実時間とのずれ %+.3f ms（最大の跳び %.3f ms）\n", sec, (diff - first_diff) / 1000.0,
                                max_jump / 1000.0);
                    max_jump = 0;
                }
            }

            if (phase == Phase::Play) {
                lane_in.clear();
                button_in.clear();
                input.collect(now_ns, lane_in, button_in);
                for (const LaneInput& li : lane_in) {
                    int64_t t = mixer.song_time_us_at(li.time_ns);
                    if (li.down) judge.press(li.lane, t);
                    else judge.release(li.lane, t);
                }
                // 入力が遅れて届く分の余裕を見て、少し手前の時刻まで進める
                judge.advance(song_us - 5000);
                for (uint16_t s : judge.sounds_to_stop()) mixer.stop_now(s);
                for (uint16_t s : judge.sounds_to_play()) mixer.play_now(s);
                judge.sounds_to_stop().clear();
                judge.sounds_to_play().clear();
                for (const JudgeEvent& je : judge.events()) {
                    view.last_judge = je.judge >= 100 ? je.judge - 100 : je.judge;
                    view.last_offset_ms = je.offset_ms;
                    view.last_fast = je.fast;
                    view.last_has_offset = je.has_offset;
                    view.last_judge_ms = je.time_us / 1000.0;
                }
                judge.events().clear();
                view.combo = judge.stats().combo;

                bool survival = !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                if (survival && judge.failed()) {
                    phase = Phase::Failed;
                    phase_start_ns = now_ns;
                    mixer.stop();
                    view.failed = true;
                } else if (song_us > chart.end_us) {
                    phase = Phase::Result;
                    mixer.stop();
                    print_result(chart, judge);
                }
            } else if (phase == Phase::Failed && now_ns - phase_start_ns > 2050'000'000ull) {
                // LR2の既定スキンの #CLOSE（2050ms）
                phase = Phase::Result;
                print_result(chart, judge);
            }

            if (phase == Phase::Result) {
                draw_result(renderer, chart, judge);
            } else {
                if (phase == Phase::Play) {
                    view.song_ms = song_us / 1000.0;
                    view.render_beat = chart.pos_at(song_us + opt.judge_timing * 1000);
                }
                char status[128];
                std::snprintf(status, sizeof(status), "UP/DOWN: HI-SPEED  LEFT/RIGHT: TIMING %+d ms  F4: FULLSCREEN  ESC: END",
                              opt.judge_timing);
                view.status = status;
                draw_play(renderer, chart, judge, view);
            }
            if (!opt.capture_dir.empty() && (song_us / 1000000 >= next_capture_s * 2 || phase == Phase::Result)) {
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
            if (opt.quit_after > 0 && (now_ns - started_ns) / 1e9 > opt.quit_after) quit = true;
        }
        mixer.stop();
    }

    mixer.close();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
