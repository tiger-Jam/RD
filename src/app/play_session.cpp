#include "app/play_session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "game/judge.h"
#include "input/input.h"
#include "render/play_scene.h"
#include "render/skin_play.h"
#include "util/path.h"

namespace bmj {

namespace {

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

}  // namespace

PlayExit run_play(App& app, const std::string& chart_path, PlayOptions opt) {
    BmsParseOptions popt;
    popt.random_seed = opt.seed ? opt.seed : static_cast<uint32_t>(SDL_GetTicksNS() % 0x7fff);
    BmsParseResult parsed = parse_bms_file(chart_path, popt);
    if (!parsed.ok) {
        std::fprintf(stderr, "読み込めない: %s\n", parsed.error.c_str());
        return PlayExit::Back;
    }
    const Chart& chart = parsed.chart;
    AppConfig& cfg = app.cfg;
    Mixer& mixer = app.mixer;

    PlayContext ctx;
    ctx.chart = &chart;
    ctx.autoplay = opt.autoplay;
    ctx.gauge_type = cfg.gauge;
    app.load_play_skin(ctx.load_options());
    Lr2Skin* skin = app.play_skin.get();
    ctx.skin = skin;
    if (skin) app.set_view(skin->width, skin->height);
    else app.set_view(640, 480);

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

    // 描画の間隔。垂直同期を使わないときは、描画を待つ間も入力と判定を回す（OpenLR2のフレームリミッタと同じ考え方）
    double fps = cfg.fps == 0 ? app.refresh_hz : cfg.fps;
    uint64_t frame_ns = (cfg.vsync || cfg.fps < 0) ? 0 : static_cast<uint64_t>(1e9 / std::max(1.0, fps));

    bool quit = false, retry = true;
    PlayExit exit = PlayExit::Back;
    while (retry && !quit) {
        retry = false;
        JudgeConfig jcfg;
        jcfg.gauge_type = cfg.gauge;
        jcfg.gas = cfg.gas;
        jcfg.autoplay = opt.autoplay;
        JudgeEngine judge(chart, jcfg);
        ctx.judge = &judge;
        ctx.timers = SkinTimers();
        ctx.song_ms = -1;
        ctx.judge_draw = 0;
        ctx.last_miss_ms = -1e9;

        InputMapper input(app.keycfg);
        PlayViewState view;
        view.autoplay = opt.autoplay;
        std::vector<LaneInput> lane_in;
        std::vector<ButtonInput> button_in;
        bool started = false;
        enum class Phase { Play, Fadeout, Failed, Result } phase = Phase::Play;
        const uint64_t scene_start = SDL_GetTicksNS();
        uint64_t prev_ns = scene_start, next_frame_ns = scene_start;
        int next_capture_s = 0;
        float scratch_angle = 0;
        int old_hp = -1;
        bool last_note_set = false, fc_set = false, done = false;
        int fps_frames = 0;
        uint64_t fps_t0 = scene_start;
        ctx.timers.set(0, 0);

        while (!done) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_EVENT_QUIT: quit = true, done = true, exit = PlayExit::Quit; break;
                case SDL_EVENT_JOYSTICK_ADDED: SDL_OpenJoystick(e.jdevice.which); break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.repeat) break;
                    if (e.key.key == SDLK_ESCAPE) {
                        if (phase == Phase::Result) done = true;
                        else phase = Phase::Result, mixer.stop(), print_result(chart, judge);
                    } else if (e.key.key == SDLK_RETURN && phase == Phase::Result) {
                        done = true;
                    } else if (e.key.key == SDLK_F5 && phase == Phase::Result) {
                        retry = true, done = true;
                    } else if (e.key.key == SDLK_F4) {
                        bool fs = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN;
                        SDL_SetWindowFullscreen(app.window, !fs);
                    } else if (e.key.key == SDLK_UP) {
                        cfg.hi_speed = std::min(900, cfg.hi_speed + 10);
                    } else if (e.key.key == SDLK_DOWN) {
                        cfg.hi_speed = std::max(10, cfg.hi_speed - 10);
                    } else if (e.key.key == SDLK_RIGHT) {
                        cfg.judge_timing = std::min(199, cfg.judge_timing + 1);
                    } else if (e.key.key == SDLK_LEFT) {
                        cfg.judge_timing = std::max(-199, cfg.judge_timing - 1);
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
            ctx.hi_speed = cfg.hi_speed;
            ctx.judge_timing = cfg.judge_timing;
            ctx.loaded = load_done.load();
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

            if (std::getenv("BMJ_DEBUG_LOOP")) {
                // 開発用：1秒あたりの入力処理の回数と描画の回数
                static uint64_t t = now_ns;
                static int loops = 0;
                ++loops;
                if (now_ns - t > 1000000000ull) {
                    std::printf("入力処理 %d回/秒、描画 %dfps\n", loops, ctx.fps);
                    loops = 0;
                    t = now_ns;
                }
            }
            // 描画の時刻でなければ、少し待って入力の処理に戻る
            if (frame_ns > 0 && now_ns < next_frame_ns) {
                SDL_DelayPrecise(std::min<uint64_t>(next_frame_ns - now_ns, 500000));
                continue;
            }
            next_frame_ns = std::max(next_frame_ns + frame_ns, now_ns);
            if (++fps_frames, now_ns - fps_t0 > 1000000000ull) {
                ctx.fps = static_cast<int>(fps_frames * 1e9 / (now_ns - fps_t0) + 0.5);
                fps_frames = 0;
                fps_t0 = now_ns;
            }

            // 皿の回転（LR2: LR2_gameloop.cpp:349。押している向きに速く、離していればゆっくり回る）
            if (input.button_held(kScratchA)) scratch_angle -= static_cast<float>(dt_ms * 0.72);
            else if (input.button_held(kScratchB)) scratch_angle += static_cast<float>(dt_ms * 0.72);
            else scratch_angle += static_cast<float>(dt_ms * 0.36);
            scratch_angle = std::fmod(scratch_angle, 360.0f);

            SDL_Renderer* renderer = app.renderer;
            double render_beat = chart.pos_at(song_us + cfg.judge_timing * 1000);
            if (phase == Phase::Result) {
                app.set_view(640, 480, false);
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
                st.hi_speed = cfg.hi_speed;
                st.autoplay = opt.autoplay;
                st.judge_draw = ctx.judge_draw;
                st.combo_draw = judge.stats().combo;
                st.gauge_hp = judge.gauge().hp(judge.gauge_type());
                st.gauge_survival = !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                st.scratch_angle = scratch_angle;
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                app.skin_renderer->draw_play(*skin, st);
            } else if (!started) {
                draw_loading(renderer, chart, load_done.load(), total_sounds);
            } else {
                view.song_ms = song_ms;
                view.render_beat = render_beat;
                view.hi_speed = cfg.hi_speed;
                char status[128];
                std::snprintf(status, sizeof(status), "UP/DOWN: HI-SPEED  LEFT/RIGHT: TIMING %+d ms  F4: FULLSCREEN  ESC: END",
                              cfg.judge_timing);
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
            if (opt.quit_after > 0 && now_ms / 1000 > opt.quit_after) quit = true, done = true, exit = PlayExit::Quit;
        }
        mixer.stop();
        if (skin) app.set_view(skin->width, skin->height, false);
    }
    if (!loader_joined) loader.join();
    return exit;
}

}  // namespace bmj
