#include "skin/bmjoy_buttons.h"
#include "app/play_session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/sound_bank.h"
#include "audio/system_sound.h"
#include "chart/bms_parser.h"
#include "chart/chart_loader.h"
#include "game/ghost.h"
#include "game/replay.h"
#include "game/graph.h"
#include "game/hispeed.h"
#include "game/judge.h"
#include "game/lane_option.h"
#include "mod/script.h"
#include "input/input.h"
#include "input/joystick_init.h"
#include "render/frame_hooks.h"
#include "render/play_scene.h"
#include "render/skin_play.h"
#include "render/text_cache.h"
#include "util/hash.h"
#include "util/path.h"
#include "video/video_decoder.h"
#include "util/dev.h"

namespace bmj {

namespace {

// キー音のファイル名を解決する（LR2: FindAltSound。書かれた名前→拡張子をogg/wavに替える→1つ上のフォルダ）
}  // namespace

std::vector<std::pair<int64_t, int64_t>> sound_ranges(const Chart& chart) {
    std::vector<std::pair<int64_t, int64_t>> r;
    for (const SoundSlice& s : chart.wav_slice) r.push_back({s.start_us, s.end_us});
    return r;
}

std::vector<std::string> resolve_chart_sounds(const Chart& chart, std::vector<std::string>& missing) {
    DirIndex dir(chart.base_dir);
    std::vector<std::string> paths(chart.wav.size());
    for (size_t i = 0; i < chart.wav.size(); ++i) {
        if (chart.wav[i].empty()) continue;
        // LR2: FindAltSound（書かれた名前 → ogg・wav・flac・mp3 → 1 つ上のフォルダで同じ手順）
        std::string p = dir.find_sound(chart.wav[i]);
        if (p.empty()) missing.push_back(chart.wav[i]);
        paths[i] = p;
    }
    return paths;
}

namespace {

// BGAの画像のファイル名を解決する（LR2: FindAltImage。書かれた名前→png/jpg/bmp→1つ上のフォルダ）
std::vector<std::string> resolve_images(const Chart& chart) {
    DirIndex dir(chart.base_dir);
    std::vector<std::string> paths(chart.bmp.size());
    for (size_t i = 0; i < chart.bmp.size(); ++i) {
        if (chart.bmp[i].empty()) continue;
        paths[i] = dir.find_image(chart.bmp[i]);
    }
    return paths;
}

// 動画の透過色（LR2: bmsload:734）。.mpg と .avi は緑、それ以外は静止画と同じく黒
int video_key(const std::string& p) {
    std::string l = p;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto ends = [&](const char* e) { return l.size() >= 3 && l.compare(l.size() - 3, 3, e) == 0; };
    return ends("mpg") || ends("avi") ? 0x00FF00 : 0x000000;
}

bool is_movie_file(const std::string& p) {
    std::string l = p;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : {".mpg", ".mpeg", ".avi", ".wmv", ".mp4", ".m4v", ".webm", ".mkv", ".ogv", ".flv"})
        if (l.size() >= std::strlen(e) && l.compare(l.size() - std::strlen(e), std::strlen(e), e) == 0) return true;
    return false;
}

// リザルトに入ったときに1回だけ記録する。途中でやめたときもLR2と同じくFAILEDとして記録する
// クリアしたときのオプションの履歴（LR2: statplay.cpp ConvertOptionHistory）。
// EASY ランプは EASY ゲージのときだけ。それより上のランプはゲージ・ランダム・HIDDEN/SUDDEN を残す
uint32_t option_history(int lamp, int gauge_type, const AppConfig& cfg) {
    if (lamp == 2) return gauge_type == kEasy ? 8u : 0u;
    if (lamp > 2) return (1u << gauge_type) | (0x100u << std::clamp(cfg.random, 0, 7)) | (0x10000u << std::clamp(cfg.effect, 0, 3));
    return 0;
}

// ゲージの絵を染める色：カスタムゲージならその色、本体のゲージなら MOD で変えた色（無ければスキンの絵のまま）。
// 1 色なら 80% 未満も同じ色。絵を描き分ける境はカスタムゲージならそのクリアの線
void set_gauge_tint(PlaySkinState& st, const JudgeEngine& j, const App& app) {
    st.gauge_tint = st.gauge_tint_low = 0;
    st.gauge_border = 80;
    if (const int c = j.custom_play(); c >= 0) {
        const CustomGaugeDef& g = j.custom_def(c);
        st.gauge_border = g.border;
        st.gauge_tint = 0xff000000u | g.color;
        st.gauge_tint_low = 0xff000000u | (g.colors == 2 ? g.color_low : g.color);
    } else if (const GaugeColors* g = app.standard_gauge_colors(j.display_gauge_type())) {
        st.gauge_tint = 0xff000000u | g->color;
        st.gauge_tint_low = 0xff000000u | (g->colors == 2 ? g->color_low : g->color);
    }
}

// 開発用：BMJ_PLAY_FRAMES のフォルダに、曲の始まりの絵を 1 フレームずつ残す（読み込み中の絵から、始まって 3 秒まで）
void dev_save_frame(SDL_Renderer* r, double ms_from_start) {
    static int no = 0;
    const char* dir = dev_env("BMJ_PLAY_FRAMES");
    if (!dir || ms_from_start > 3000 || no >= 400) return;
    char name[512];
    std::snprintf(name, sizeof(name), "%s/p%04d_%05d.bmp", dir, no++, static_cast<int>(ms_from_start));
    if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, name), SDL_DestroySurface(sf);
}

// lamp_only：ランプ・回数・オプション履歴だけを残す（CONSTANT・SCATTER・CONVERGE。LR2: statplay.cpp:895）
// ghost は判定の列、rseed は譜面の乱数の種（自己ベストを更新したときだけ残る）
// entry は 1 回ごとのプレーの表に残す分（ずれの統計と入力の記録の場所。ここでゲージなどを足す）
void record_score(App& app, const std::string& sha256, const JudgeEngine& j, int lamp, const AppConfig& cfg, bool lamp_only, bool count_only, bool course_stage, int ln_mode, int profile, const std::vector<int8_t>& ghost,
                  uint32_t rseed, PlayEntry entry) {
    const PlayStats& s = j.stats();
    PlayRecord rec;
    rec.count_only = count_only;
    rec.ghost = encode_ghost(ghost);
    rec.rseed = static_cast<int>(rseed);
    rec.op_best = j.gauge_type() + std::clamp(cfg.random, 0, 9) * 10;  // LR2: ゲージ + 配置×10
    rec.course_stage = course_stage;
    rec.lamp_only = lamp_only;
    rec.ln_mode = ln_mode;
    rec.profile = profile;
    rec.clear = lamp, rec.exscore = s.exscore(), rec.max_combo = s.max_combo;
    rec.judge = {s.judge_count[kPGreat], s.judge_count[kGreat], s.judge_count[kGood], s.judge_count[kBad], s.judge_count[kPoor]};
    rec.op_history = option_history(lamp, j.gauge_type(), cfg);
    // 処理しなかったノーツは、どう終わっても BP に入れる（途中でやめたときも閉店したときも。LR2: statplay.cpp:992）
    rec.bp = s.bp() + s.total_notes - s.note_current;
    app.db.save_score(sha256, rec);
    // カスタムゲージで遊んだときは、そのゲージのランプも残す（ランプは遊んでいるゲージだけで決める）。
    // 記録の鍵はカスタムランプの鍵（同じランプのゲージどうしは同じ記録）。本体のランプを付けるゲージなら、ふだんの記録だけ
    const int c = j.custom_play();
    if (c >= 0 && !lamp_only && !count_only && !app.custom_gauges()[c].second->lamp_key.empty()) {
        int clamp = lamp >= 5 ? lamp - 2 : j.custom_cleared(c) ? 2 : 1;
        app.db.save_custom_lamp(sha256, ln_mode, profile, app.custom_gauges()[c].second->lamp_key, clamp);
    }
    // 1 回ごとのプレーの表には、全部のプレーを残す。遅くして遊んだときは、ふだんの記録には回数しか残さない代わりに、中身はここに残る
    static const char* names[kGaugeCount] = {"GROOVE", "HARD", "HAZARD", "EASY", "P-ATTACK", "G-ATTACK"};
    entry.kind = count_only ? "slow" : course_stage ? "course" : "";
    entry.rate = cfg.sound_fx.play_rate();
    entry.gauge = c >= 0 ? app.custom_gauges()[c].first : names[j.gauge_type()];
    entry.fast = s.fast, entry.slow = s.slow;
    char buf[64];
    for (int g = 0; g < kGaugeCount; ++g) {
        std::snprintf(buf, sizeof(buf), "%s%s=%.1f", g ? "," : "", names[g], j.gauge().hp(g));
        entry.gauges += buf;
    }
    const auto keys = app.custom_gauges();
    for (int i = 0; i < j.custom_count() && i < static_cast<int>(keys.size()); ++i) {
        std::snprintf(buf, sizeof(buf), "=%.1f", j.custom_hp(i));
        entry.gauges += "," + keys[i].first + buf;
    }
    app.db.log_play(sha256, rec, entry);
}

void print_result(const Chart& chart, const JudgeEngine& j) {
    const PlayStats& s = j.stats();
    static const char* lamps[] = {"NO PLAY", "FAILED", "EASY", "NORMAL", "HARD", "FULL COMBO", "PERFECT"};
    std::printf("結果: %s\n", chart.meta.title.c_str());
    std::printf("  ランプ %s / EX %d / MAX COMBO %d / BP %d\n", lamps[j.failed() ? 1 : j.clear()], s.exscore(),
                s.max_combo, s.bp());
    std::printf("  PG %d GR %d GD %d BD %d PR %d (空POOR %d) FAST %d SLOW %d / %d ノーツ\n", s.judge_count[kPGreat],
                s.judge_count[kGreat], s.judge_count[kGood], s.judge_count[kBad], s.judge_count[kPoor], s.empty_poor,
                s.fast, s.slow, s.total_notes);
    std::printf("  ゲージ %.1f%%\n", j.play_hp());
}

}  // namespace

// ハイスピード固定（LR2: bmsload:3314）。ノーツのある BPM を基準に、BPM 150 と同じ速さに見えるように掛ける
double hs_fix_multiplier(const Chart& chart, int hs_fix) {
    if (hs_fix == 0) return 1.0;
    double sum = 0, max_bpm, min_bpm;
    note_bpm_range(chart, min_bpm, max_bpm);
    int count = 0;
    for (const Note& n : chart.notes) {
        if (n.kind == NoteKind::Invisible || n.kind == NoteKind::Mine) continue;
        sum += chart.bpm_at(n.time_us);
        count++;
    }
    if (count == 0) return 1.0;
    switch (hs_fix) {
    case 1: return max_bpm > 0 ? 150.0 / max_bpm : 1.0;
    case 2: return min_bpm > 0 ? 150.0 / min_bpm : 1.0;
    case 3: return sum > 0 ? 150.0 / (sum / count) : 1.0;
    case 5: {
        // ノーツがいちばん多い BPM（OpenLR2 の拡張）
        double m = main_bpm(chart);
        return m > 0 ? 150.0 / m : 1.0;
    }
    default: return 1.0;
    }
}

namespace {

// 譜面のファイルを読んで、原本の SHA-256 と、#RANDOM を評価した譜面を作る
std::pair<std::string, BmsParseResult> read_chart(const std::string& path, uint32_t seed) {
    if (const char* d = dev_env("BMJ_CHART_DELAY")) SDL_Delay(static_cast<Uint32>(std::atoi(d)));  // 開発用：冷えた HDD のまね
    std::string sha256;
    {
        std::ifstream f(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sha256 = sha256_hex(bytes);
    }
    BmsParseOptions popt;
    popt.random_seed = seed;
    return {sha256, parse_chart_file(path, popt)};
}

}  // namespace

PlayExit run_play(App& app, const std::string& chart_path, PlayOptions opt) {
    AppConfig& cfg = app.cfg;
    // メトロノームを鳴らして遊んだときは、記録（ランプ・スコア・回数・リプレイ）を残さない（オートプレイと同じ扱い）
    if (app.metronome()) opt.save_score = false;
    const uint64_t prelude_ns = SDL_GetTicksNS();
    // 譜面のファイルは裏で読み始める（リプレイの再生と開発用の入力は、記録の乱数の種で読むので、この下で読む）
    const bool read_early = opt.record && !opt.replay && !dev_env("BMJ_INPUT_REPLAY");
    std::future<std::pair<std::string, BmsParseResult>> early;
    uint32_t chart_seed = opt.seed ? opt.seed : static_cast<uint32_t>(SDL_GetTicksNS() % 0x7fff);  // #RANDOM の乱数の種
    if (read_early) early = std::async(std::launch::async, read_chart, chart_path, chart_seed);
    // 読み終えるのに時間がかかるとき（冷えた HDD など）は、先にプレー画面のスキンを読んで、読み込み中の絵を出して待つ。
    // スキンの #IF は譜面で決まるものもあるので、選曲画面の譜面の情報で仮に決め、読み終えて違っていたら読み直す
    bool prelude_shown = false;
    std::array<bool, 1000> prelude_ops{};
    if (read_early && early.wait_for(std::chrono::milliseconds(80)) != std::future_status::ready) {
        const ChartRecord& r = *opt.record;
        Chart pc;
        pc.meta.title = r.title, pc.meta.subtitle = r.subtitle, pc.meta.artist = r.artist, pc.meta.subartist = r.subartist;
        pc.meta.genre = r.genre, pc.meta.playlevel = r.playlevel, pc.meta.difficulty = r.difficulty;
        pc.judge_rank = r.rank;
        pc.initial_bpm = r.bpm_main > 0 ? r.bpm_main : r.bpm_max;
        if (r.bpm_min > 0) pc.scroll.push_back(ScrollSeg{}), pc.scroll.back().bpm = r.bpm_min;
        if (r.bpm_max > 0) pc.scroll.push_back(ScrollSeg{}), pc.scroll.back().bpm = r.bpm_max;
        PlayContext pctx;
        pctx.chart = &pc;
        pctx.autoplay = opt.autoplay;
        pctx.bga_shown = cfg.auto_adjust != 2 && (cfg.bga == 1 || cfg.bga == 3 || (cfg.bga == 2 && opt.autoplay));
        pctx.hs_fix = cfg.hs_fix;
        pctx.gauge_type = cfg.gauge;
        pctx.ghost_pos = cfg.ghost, pctx.score_graph = cfg.score_graph, pctx.bga_size = cfg.bga_size;
        pctx.silent = cfg.auto_adjust == 2;
        pctx.replay_rec = !opt.autoplay && !opt.course && cfg.replay_save > 0;
        pctx.has_stagefile = !r.stagefile.empty(), pctx.has_backbmp = !r.backbmp.empty();
        ScoreRecord b;
        if (app.db.get_score(r.sha256, b, r.ln_free > 0 ? cfg.ln_mode : 0, app.mod_profile()))
            pctx.best_clear = b.clear, pctx.best_exscore = b.exscore;
        prelude_ops = pctx.load_options();
        app.load_play_skin(prelude_ops);
        prelude_shown = true;
        if (Lr2Skin* sk = app.play_skin.get()) {
            app.set_view(sk->width, sk->height);
            SDL_HideCursor();
            pctx.skin = sk;
            pctx.timers.set(0, 0);
            while (early.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    // 待っている間に閉じられても、読み終えるまでは待つ（読んでいる途中で捨てられないので）
                    if (e.type == SDL_EVENT_JOYSTICK_ADDED) SDL_OpenJoystick(e.jdevice.which);
                }
                PlaySkinState st;
                st.now_ms = (SDL_GetTicksNS() - prelude_ns) / 1e6;
                st.timers = &pctx.timers;
                st.option = [&](int o) { return pctx.option(o); };
                st.number = [&](int id) { return pctx.number(id); };
                st.bargraph = [&](int t, double& v, double& m) { return pctx.bargraph(t, v, m); };
                st.slider = [&](int t, int& v, int& lo, int& hi) { return pctx.slider(t, v, lo, hi); };
                st.text = [&](int id) { return pctx.text(id); };
                st.button = [&](int t) { return pctx.button(t); };
                if (auto it = cfg.skin_adjust.find(sk->path); it != cfg.skin_adjust.end()) st.adjust = it->second;
                SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
                SDL_RenderClear(app.renderer);
                app.skin_renderer->draw_play(*sk, st);
                dev_save_frame(app.renderer, (SDL_GetTicksNS() - prelude_ns) / 1e6);
                frame_present(app.renderer);
            }
        }
        std::printf("譜面の読み込みを待った：%.0f ms\n", (SDL_GetTicksNS() - prelude_ns) / 1e6);
    }
    // スコアは譜面ファイルの原本のSHA-256で記録する（仕様書8章）
    std::string sha256;
    BmsParseResult parsed;
    if (read_early) {
        auto got = early.get();
        sha256 = std::move(got.first), parsed = std::move(got.second);
    } else {
        std::ifstream f(chart_path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sha256 = sha256_hex(bytes);
    }
    // リプレイの再生：記録したときの乱数の種とオプションで遊ぶ。オプションは終わったら戻す（LR2: SetReplayConfig）
    Replay replay;
    const AppConfig saved_cfg = cfg;
    struct Restore {
        std::function<void()> f;
        ~Restore() { f(); }
    } restore_replay{[&] {
        if (!opt.replay) return;
        cfg.random = saved_cfg.random, cfg.gauge = saved_cfg.gauge, cfg.ln_mode = saved_cfg.ln_mode, cfg.hs_fix = saved_cfg.hs_fix;
        cfg.effect = saved_cfg.effect, cfg.seven_key = saved_cfg.seven_key, cfg.gas = saved_cfg.gas, cfg.hi_speed = saved_cfg.hi_speed;
        cfg.lane_cover = saved_cfg.lane_cover, cfg.lane_cover_v = saved_cfg.lane_cover_v, cfg.judge_timing = saved_cfg.judge_timing;
        cfg.custom_gauge = saved_cfg.custom_gauge;
    }};
    // 開発用：BMJ_INPUT_REPLAY のファイルの入力を、手で押したものとして流す（記録・ゴースト・リプレイの保存はふだんどおり）
    bool feed_input = false;
    if (const char* f = dev_env("BMJ_INPUT_REPLAY"); f && !opt.replay && !opt.autoplay) {
        feed_input = load_replay(f, replay);
        std::printf("開発用：入力を流す %s（%zu 件）\n", feed_input ? "" : "読めない", replay.events.size());
    }
    if (opt.replay) {
        // 配置の作り方の版が違う記録は、同じ譜面を作れないので再生しない（古い版の作り方を残したら、ここで選ぶ）
        // プレーに影響する MOD の組み合わせが今と違う記録も、同じ判定にならないので再生しない
        if (!load_replay(replay_path(sha256), replay) || replay.sha256 != sha256 || replay.random_version != kRandomAlgorithmVersion ||
            replay.mods != play_mod_key(app.active_mods())) {
            std::fprintf(stderr, "リプレイが無い: %s\n", chart_path.c_str());
            opt.replay = false;
            return PlayExit::Back;
        }
        cfg.random = replay.random, cfg.gauge = replay.gauge, cfg.ln_mode = replay.ln_mode, cfg.hs_fix = replay.hs_fix;
        cfg.effect = replay.effect, cfg.seven_key = replay.seven_key, cfg.gas = replay.gas, cfg.hi_speed = replay.hi_speed;
        cfg.lane_cover = replay.lane_cover, cfg.lane_cover_v = replay.lane_cover_v, cfg.custom_gauge = replay.custom_gauge;
        opt.seed = replay.seed, opt.save_score = false, opt.autoplay = false;
    }
    if (!read_early) {
        BmsParseOptions popt;
        chart_seed = opt.seed ? opt.seed : chart_seed;  // リプレイの再生は、記録した乱数の種で読む
        popt.random_seed = chart_seed;
        parsed = parse_chart_file(chart_path, popt);
    }
    if (!parsed.ok) {
        // 曲データベースにはあるが、ファイルが無い（消した・別の PC へ持ってきた）ときもここ。選曲画面へ戻る
        std::fprintf(stderr, "譜面を読み込めない（ファイルが無いなら、F8 か曲データベースの再作成で読み直す）: %s\n", parsed.error.c_str());
        return PlayExit::Back;
    }
    // 7k モード：皿のある 7 鍵の譜面は、皿のノーツを除いて遊ぶ（記録も別）。皿の無い譜面・5 鍵の譜面はふだんどおり（記録も同じ）
    const bool seven_key = cfg.seven_key && parsed.chart.layout == KeyLayout::K7S && key_count(parsed.chart) == 7;
    if (seven_key) parsed.chart.drop_scratch();
    // 配置のオプション（LR2: bmsload:3354）。#RANDOM を評価した乱数の続きで並べ替える
    apply_random_option(parsed.chart, cfg.random, parsed.rng, !seven_key);
    // TOTAL の既定式は、並べ替えで重なったノーツを捨てたあとの数で（LR2: bmsload:3789）
    if (!parsed.chart.total_defined) parsed.chart.total = lr2_default_total(parsed.chart.judgeable_notes());
    // HS-FIX の CONSTANT：一定の速さで流す
    const bool constant = cfg.hs_fix == 4;
    if (constant) parsed.chart.make_constant_speed();
    // CONSTANT・SCATTER・CONVERGE は、ランプを EASY まで（フルコンボも EASY）にして、
    // ランプ・回数・オプション履歴だけを残す。コースのステージでは何も残さない（LR2: statplay.cpp:7, 895）
    const bool lamp_only = constant || cfg.random >= kScatter;
    // 曲を遅くして（ピッチの FREQ・SPEED で下げて）遊んだら、回数だけを残す（LR2: statplay.cpp:791。速くしたときは普通に残す）
    const bool slowed = cfg.sound_fx.play_rate() < 1.0;
    // BGA の無い譜面は、LR2files/Movie の動画を 1 本ランダムに選んで、頭から繰り返し流す（LR2: bmsload:4089。コースでは流さない）。
    // 探す順は mpg → avi → wmv → mp4 → ogv で、最初に見つかった種類の中から選ぶ
    // BGA の設定（LR2: bga）。OFF なら画像も動画も読まない。NO MISS はミスのときの絵を出さない
    // 判定タイミングの自動調整の SILENT は、キー音も BGA も読まない（LR2: bmsload:659）
    const bool silent = cfg.auto_adjust == 2;
    const bool bga_shown = !silent && (cfg.bga == 1 || cfg.bga == 3 || (cfg.bga == 2 && opt.autoplay));
    std::string default_movie;
    int default_movie_id = -1;
    if (bga_shown && parsed.chart.sound_only && !opt.course && VideoDecoder::available()) {
        for (const char* pat : {"LR2files/Movie/*.mpg", "LR2files/Movie/*.avi", "LR2files/Movie/*.wmv", "LR2files/Movie/*.mp4",
                                "LR2files/Movie/*.ogv"})
            if (!app.lr2_root.empty() && !(default_movie = resolve_lr2_path(app.lr2_root, pat, true)).empty()) break;
        if (!default_movie.empty()) {
            if (parsed.chart.bmp.empty()) parsed.chart.bmp.push_back("");
            default_movie_id = static_cast<int>(parsed.chart.bmp.size());
            parsed.chart.bmp.push_back("");
            BgaEvent e;
            e.time_us = 0;
            e.layer = BgaEvent::Layer::Base;
            e.image = static_cast<uint16_t>(default_movie_id);
            parsed.chart.bga.insert(parsed.chart.bga.begin(), e);
        }
    }
    const Chart& chart = parsed.chart;
    // ハイスピのモード（仕様書6章）。フローティングは HS-FIX の倍率を使わず、基準の BPM で緑数字を合わせる
    const bool floating = cfg.hs_mode == 1;
    const double speed_multiplier = floating ? 1.0 : hs_fix_multiplier(chart, cfg.hs_fix);
    // ピッチの FREQ・SPEED で曲を速くしても遅くしても、ノーツが画面を流れる速さ（緑数字）は普段と同じにする。
    // 拍は曲の速さで進むので、1 拍の長さを曲の速さで割る（LR2 は曲と一緒に流れも速くなる。BMjoy の変更）
    const double scroll_multiplier = speed_multiplier / cfg.sound_fx.play_rate();
    const double fhs_bpm = constant ? 150.0 : floating_base_bpm(chart, cfg.fhs_base);
    int fhs = 100;  // フローティングのハイスピ（100 = 1.00）
    int& hs = floating ? fhs : cfg.hi_speed;
    int start_green = cfg.fhs_green;  // 曲の始まりの緑数字（ギアチェンの「緑数字に戻す」で使う）
    // 記録は LN の種類ごとに分ける（種類を決めていない LN が無い譜面は 0 にまとめる）
    const int score_ln_mode = chart.free_ln_count() > 0 ? cfg.ln_mode : 0;
    // プレーに影響する MOD（判定など）を使っているときは、組み合わせごとに別の記録にする。7k モードも別の記録
    const int profile = app.mod_profile(seven_key);
    ScoreRecord best_score;
    bool has_best = app.db.get_score(sha256, best_score, score_ln_mode, profile);
    Mixer& mixer = app.mixer;

    // 決定の音（LR2では決定画面で鳴る。BMjoyには決定画面が無いので、読み込みの間に鳴らす）
    if (opt.decide_sound) mixer.play_system(kSysDecide);
    // 演奏中はマウスカーソルを出さない（LR2 と同じ。選曲画面に戻れば選曲画面が決め直す）
    SDL_HideCursor();
    PlayContext ctx;
    ctx.chart = &chart;
    CourseRun* course = opt.course;
    if (course) {
        ctx.course = course->course;
        ctx.course_stage = course->stage;
        ctx.course_stages = static_cast<int>(course->course->stages.size());
    }
    ctx.autoplay = opt.autoplay;
    ctx.bga_shown = bga_shown;
    ctx.hs_fix = cfg.hs_fix;
    {
        // 同じフォルダの、難易度ごとの譜面のレベル（数値45〜49）
        // 曲データベースのパスは絶対パスなので、合わせてから比べる
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(chart_path, ec).lexically_normal();
        std::string folder = abs.parent_path().string();
        for (const ChartRecord& c : app.db.charts_in_folder(folder)) {
            if (c.difficulty < 1 || c.difficulty > 5) continue;
            int& lv = ctx.diff_level[c.difficulty - 1];
            if (lv < 0 || c.path == abs.string()) lv = c.playlevel;
        }
        // 曲データベースに無い譜面でも、自分の難易度のところには自分のレベルを入れる
        int d = chart.meta.difficulty;
        if (d >= 1 && d <= 5) ctx.diff_level[d - 1] = chart.meta.playlevel;
    }
    ctx.gauge_type = cfg.gauge;
    // スキンの #IF は読み込むときに決まるので、設定で決まる条件（スコアグラフ・ゴーストの位置・BGA の大きさ・リプレイ）は先に入れる
    ctx.ghost_pos = cfg.ghost;
    ctx.score_graph = cfg.score_graph;
    ctx.bga_size = cfg.bga_size;
    ctx.silent = silent;
    ctx.replay = opt.replay;
    ctx.replay_rec = !opt.replay && !opt.autoplay && !course && cfg.replay_save > 0;
    // 自己ベストのランプ（op 100〜105）も読み込みの条件になる（WMIX はこれで選んだ絵を、リザルトで更新前のランプに使う）
    ctx.best_clear = has_best ? best_score.clear : 0;
    ctx.best_exscore = has_best ? best_score.exscore : 0;
    {
        // 読み込み中の絵を出すために仮の条件で読んだスキンは、本当の譜面で条件が同じならそのまま使う
        const std::array<bool, 1000> ops = ctx.load_options();
        if (prelude_shown && ops != prelude_ops) {
            // 仮の条件と違ったもの（読み直しで絵が切り替わって見えるので、どれが違ったかを残す）
            std::string diff;
            for (int i = 0; i < 1000; ++i)
                if (ops[i] != prelude_ops[i]) diff += " " + std::to_string(i) + (ops[i] ? "+" : "-");
            std::printf("読み込み中の絵のスキンを読み直す（条件が違った:%s）\n", diff.c_str());
        }
        if (!prelude_shown || ops != prelude_ops) app.load_play_skin(ops);
        std::printf("曲の始まり：スキンまで %.0f ms\n", (SDL_GetTicksNS() - prelude_ns) / 1e6);
    }
    Lr2Skin* skin = app.play_skin.get();
    ctx.skin = skin;
    // 判定ラインまでのレーンの長さ（スキンの座標）。緑数字と LIFT の計算に使う
    const double lane_px = skin && !skin->note_dst[1].empty() ? skin->note_dst[1].keys.front().y : 480.0;
    if (skin) app.set_view(skin->width, skin->height);
    else app.set_view(640, 480);

    // キー音はワーカーで読み込む。読み込み中もスキンの読み込み画面を動かす
    std::vector<std::string> missing;
    std::vector<std::string> paths = resolve_chart_sounds(chart, missing);
    // メトロノーム（MOD）：小節の頭と拍の音を、譜面の音の後ろの番号に足し、拍の時刻に BGM と同じく鳴らす
    //（曲と同じ時計で鳴るので、FREQ で速さを変えても拍からずれない）
    const ModManifest* metro = app.metronome();
    int metro_accent = -1, metro_beat = -1;
    std::vector<Chart::Beat> metro_beats;
    if (metro && paths.size() + 2 <= 0xffff) {
        metro_accent = static_cast<int>(paths.size()), paths.push_back(metro->metronome_accent);
        metro_beat = static_cast<int>(paths.size()), paths.push_back(metro->metronome_beat);
        metro_beats = chart.metronome_beats();
        std::printf("メトロノーム：%zu 拍（音量 %d%%）\n", metro_beats.size(), metro->metronome_volume);
    }
    if (silent) paths.assign(paths.size(), ""), missing.clear();
    std::vector<std::pair<int64_t, int64_t>> ranges = sound_ranges(chart);
    for (const auto& m : missing) std::printf("キー音が見つからない: %s\n", m.c_str());
    int total_sounds = static_cast<int>(std::count_if(paths.begin(), paths.end(), [](auto& p) { return !p.empty(); }));
    // BGAの画像と動画（動画は FFmpeg が読み込めたときだけ出す）
    std::vector<std::string> bga_paths = resolve_images(chart);
    if (!bga_shown) bga_paths.assign(bga_paths.size(), "");
    if (default_movie_id >= 0) bga_paths[default_movie_id] = default_movie;
    std::vector<std::unique_ptr<VideoDecoder>> videos(bga_paths.size());
    int total_images = 0;
    for (std::string& p : bga_paths) {
        if (!p.empty() && is_movie_file(p) && !VideoDecoder::available()) p.clear();
        if (!p.empty()) total_images++;
    }
    std::vector<SkinImage> bga_images(bga_paths.size());
    // 譜面に書かれた STAGEFILE / BACKBMP（スキンの特別な画像 100 / 101）
    std::string stage_path, back_path;
    {
        DirIndex dir(chart.base_dir);
        if (!chart.meta.stagefile.empty()) stage_path = dir.resolve(chart.meta.stagefile, {".png", ".jpg", ".bmp"});
        if (!chart.meta.backbmp.empty()) back_path = dir.resolve(chart.meta.backbmp, {".png", ".jpg", ".bmp"});
    }
    SkinImage stage_image, back_image;
    SoundBank bank;
    std::vector<std::string> load_errors;
    std::atomic<bool> loaded{false};
    std::atomic<int> load_done{0};
    std::thread loader([&] {
        int threads = std::max(2u, std::thread::hardware_concurrency() / 2) + 1;
        bank.load(paths, mixer.sample_rate(), threads, &load_errors, &load_done, &ranges, cfg.sound_16bit);
        if (metro_accent >= 0) {
            const float gain = metro->metronome_volume / 100.0f;
            bank.scale(static_cast<size_t>(metro_accent), gain), bank.scale(static_cast<size_t>(metro_beat), gain);
        }
        // BGAは黒を透過色にする（LR2: bmsload:731 SetTransColor(0,0,0)）
        const uint8_t black[3] = {0, 0, 0};
        for (size_t i = 0; i < bga_paths.size(); ++i) {
            if (bga_paths[i].empty()) continue;
            if (is_movie_file(bga_paths[i])) {
                auto v = std::make_unique<VideoDecoder>();
                v->set_loop(static_cast<int>(i) == default_movie_id);
                if (v->open(bga_paths[i], video_key(bga_paths[i]))) videos[i] = std::move(v);
                else std::printf("動画を開けない: %s\n", bga_paths[i].c_str());
            } else {
                load_skin_image(bga_paths[i], bga_images[i], black, true);
            }
            load_done.fetch_add(1);
        }
        if (!stage_path.empty()) load_skin_image(stage_path, stage_image, black, false);
        if (!back_path.empty()) load_skin_image(back_path, back_image, black, false);
        loaded = true;
    });
    bool loader_joined = false;
    ctx.load_total = std::max(1, total_sounds + total_images);
    std::vector<SDL_Texture*> bga_textures;
    SDL_Texture* stage_tex = nullptr;
    SDL_Texture* back_tex = nullptr;

    // 描画の間隔。垂直同期を使わないときは、描画を待つ間も入力と判定を回す（OpenLR2のフレームリミッタと同じ考え方）
    double fps = cfg.fps == 0 ? app.refresh_hz : cfg.fps;
    uint64_t frame_ns = (cfg.vsync || cfg.fps < 0) ? 0 : static_cast<uint64_t>(1e9 / std::max(1.0, fps));

    TextCache overlay_text(app.renderer);  // スキンの位置の調整の表示
    bool quit = false, retry = true, retried = false;
    PlayExit exit = PlayExit::Back;
    for (; retry && !quit; retried = true) {
        retry = false;
        JudgeConfig jcfg;
        jcfg.gauge_type = cfg.gauge;
        jcfg.gas = cfg.gas;
        jcfg.autoplay = opt.autoplay;
        // MOD：カスタムゲージは全部を裏で計算し、選んでいればそれで遊ぶ。カスタム判定は 1 つだけ。GAS は MOD でも入れられる
        const auto custom_gauges = app.custom_gauges();
        for (size_t i = 0; i < custom_gauges.size(); ++i) {
            jcfg.custom_gauges.push_back(*custom_gauges[i].second);
            if (custom_gauges[i].first == cfg.custom_gauge) jcfg.custom_play = static_cast<int>(i);
        }
        for (const ModManifest* m : app.active_mods())
            if (const CustomJudgeDef* j = m->active_judge(); j && !jcfg.custom_judge) jcfg.custom_judge = *j;
        // GAS：GAUGE で「GAS」を選んでいれば、GAS の MOD の設定の順番で落ちていく（MOD を入れただけでは効かない）
        if (cfg.custom_gauge == kGasKey) {
            for (const std::string& k : app.gas_order()) {
                for (int i = 0; i < kGaugeCount; ++i)
                    if (k == kGaugeKeys[i]) jcfg.gas_chain.push_back(i);
                for (size_t i = 0; i < custom_gauges.size(); ++i)
                    if (k == custom_gauges[i].first) jcfg.gas_chain.push_back(kGaugeCount + static_cast<int>(i));
            }
            if (!jcfg.gas_chain.empty() && jcfg.gas_chain[0] < kGaugeCount) jcfg.gauge_type = jcfg.gas_chain[0];
        }
        jcfg.ln_mode = cfg.ln_mode;
        if (course) {
            // 2 ステージ目からは、前のステージのゲージ・ゲージの種類・コンボを引き継ぐ
            jcfg.course = course->course->type;
            jcfg.carry = course->stage > 0;
            jcfg.carry_hp = course->hp;
            jcfg.carry_combo = course->combo;
            jcfg.carry_max_combo = course->max_combo;
            jcfg.gauge_type = course->gauge_type;
            jcfg.no_good = course->course->no_good;
            jcfg.no_great = course->course->no_great;
            // コースでは GAS（ゲージの自動切り替え）を使わない。コースは選んだゲージで最後まで遊び、
            // そのゲージが尽きたら終わり（途中で甘いゲージへ移ると、段位の合格の意味が変わる。LR2 には GAS が無い）
            jcfg.gas = false;
            jcfg.gas_chain.clear();
        }
        JudgeEngine judge(chart, jcfg);
        ctx.judge = &judge;
        // MOD のスクリプト（プレーごとに作り直す。ゲームの状態は写しを渡すだけ）
        ModScripts scripts;
        scripts.load(app.active_mods(), mod_data_dir(""));
        auto report_mod_errors = [&] {
            for (const std::string& e : scripts.take_errors()) std::printf("MOD が止まった：%s\n", e.c_str());
        };
        if (!scripts.empty()) {
            ModPlayInfo info;
            info.sha256 = sha256, info.path = chart_path;
            info.title = chart.meta.title, info.subtitle = chart.meta.subtitle, info.artist = chart.meta.artist;
            info.genre = chart.meta.genre, info.playlevel = chart.meta.playlevel;
            info.notes = judge.stats().total_notes, info.total = static_cast<int>(chart.total), info.rank = chart.judge_rank;
            info.keys = key_count(chart), info.length_ms = static_cast<int>(chart.end_us / 1000);
            info.bpm_min = info.bpm_max = chart.initial_bpm;
            for (const ScrollSeg& sg : chart.scroll)
                if (sg.bpm > 0) info.bpm_min = std::min(info.bpm_min, sg.bpm), info.bpm_max = std::max(info.bpm_max, sg.bpm);
            static const char* gauge_names[kGaugeCount] = {"GROOVE", "HARD", "HAZARD", "EASY", "P-ATTACK", "G-ATTACK"};
            info.gauge = judge.custom_play() >= 0 ? cfg.custom_gauge : gauge_names[judge.gauge_type()];
            info.random = cfg.random, info.autoplay = opt.autoplay, info.replay = opt.replay, info.course = course != nullptr;
            info.view_w = skin ? skin->width : app.view_w, info.view_h = skin ? skin->height : app.view_h;
            const JudgeWindows& w = judge.windows();
            info.pgreat[0] = w.pgreat, info.pgreat[1] = w.pgreat_late, info.great[0] = w.great, info.great[1] = w.great_late;
            info.good[0] = w.good, info.good[1] = w.good_late, info.bad[0] = w.bad, info.bad[1] = w.bad_late;
            scripts.play_start(info);
            report_mod_errors();
        }
        ctx.timers = SkinTimers();
        ctx.song_ms = -1;
        ctx.judge_draw = 0;
        ctx.last_miss_ms = -1e9;
        // リトライ：リザルトで切り替えたものをプレーの状態に戻す（戻さないと、スキンは読み込み中の絵を出し続ける）
        ctx.in_result = false, ctx.quit_early = false, ctx.result_clear = 0, ctx.result_flip = false;
        ctx.skin = skin;
        // 自己ベストとターゲットのゴースト（LR2: Scene04_Play.cpp:1969）。リトライでは、記録し直した自己ベストを読み直す
        if (retried) has_best = app.db.get_score(sha256, best_score, score_ln_mode, profile);
        const int total_notes = judge.stats().total_notes;
        ctx.best = Ghost{};
        if (has_best && !best_score.ghost.empty()) ctx.best.set_from_judges(decode_ghost(best_score.ghost));
        // 判定の列が無い記録（列を残す前に BMjoy で遊んだもの）は、EX スコアから作る
        else if (has_best && best_score.exscore > 0) ctx.best.set_from_exscore(best_score.exscore, total_notes);
        ctx.best_clear = has_best ? best_score.clear : 0;
        ctx.best_exscore = has_best ? best_score.exscore : 0;
        // ターゲット：0=既定（2P 側は動かさない） 1=自己ベスト（無ければ既定の割合） 2〜4=AAA/AA/A 5=割合
        switch (cfg.target) {
        case 1:
            ctx.target.set_default(cfg.target_percent, total_notes);
            ctx.target_type = ctx.best.final_exscore() > 0 ? 1 : 2;
            break;
        case 2: case 3: case 4:
            ctx.target.set_default(cfg.target == 2 ? 88 : cfg.target == 3 ? 77 : 66, total_notes);
            ctx.target_type = 2;
            break;
        case 5:
            ctx.target.set_default(cfg.target_percent, total_notes);
            ctx.target_type = 2;
            break;
        default:
            ctx.target.set_default(cfg.target_percent, total_notes);
            ctx.target_type = 0;
            break;
        }
        ctx.ghost_pos = cfg.ghost;
        ctx.score_graph = cfg.score_graph;
        ctx.bga_size = cfg.bga_size;
        ctx.sound_fx = &cfg.sound_fx;
        ctx.silent = silent;
        ctx.replay = opt.replay;
        ctx.replay_rec = !opt.replay && !opt.autoplay && !course && cfg.replay_save > 0;
        // このプレイの判定の列（自己ベストを更新したらゴーストとして残す）と、リプレイに残す入力
        std::vector<int8_t> ghost_log;
        // 判定のずれの統計（PGREAT〜BAD のずれの和と二乗の和。1 回ごとのプレーの表に残す）
        int offset_n = 0;
        double offset_sum = 0, offset_sq = 0;
        Replay record;
        size_t replay_pos = 0;
        bool audio_fading = false;  // リザルトを抜ける音のフェードを始めた

        InputMapper input(app.keycfg);
        input.set_min_hold_ms(cfg.input_interval);
        PlayViewState view;
        view.autoplay = opt.autoplay;
        std::vector<LaneInput> lane_in;
        std::vector<ButtonInput> button_in;
        bool started = false;
        enum class Phase { Play, Fadeout, Failed, Result } phase = Phase::Play;
        // 譜面を待つ間に読み込み中の絵を出していたら、その続きから動かす（入場の演出を頭からやり直さない）
        const uint64_t scene_start = prelude_shown && !retried ? prelude_ns : SDL_GetTicksNS();
        uint64_t prev_ns = scene_start, next_frame_ns = scene_start;
        int next_capture_s = 0;
        float scratch_angle = 0;
        // レーンカバーを出しているか（LR2: lanecoverDisplay。START か SELECT の2度押しで切り替える）
        bool cover_display = true;
        double cover_dbl_ms = 0;
        int old_hp = -1;
        // 判定タイミングの自動調整（LR2: Play.cpp:1319）。PG〜BD で押すたびにずれを足し、10 回たまったら
        // 遅ければ +1ms、早ければ −1ms 動かす。オートプレイでは動かさない
        int adjust_count = 0, adjust_sum = 0;
        bool last_note_set = false, fc_set = false, done = false;
        // BGAの層（LR2: bmsload:597, 810）。#BMP00 があれば、それが最初のPOORの画像になる
        int bga_base = -1, bga_layer = -1, bga_poor = -1;
        bool bga_poor_init = false;
        size_t bga_next = 0;
        // 動画の再生を始めた曲の時刻（ms。まだなら負）。もう一度遊ぶときは頭から開き直す
        std::vector<double> video_start(videos.size(), -1.0);
        if (loader_joined)
            for (auto& v : videos)
                if (v) v->restart();
        double last_miss_ms = -1e9;
        int fps_frames = 0;
        uint64_t fps_t0 = scene_start;
        bool recorded = false;
        PlayGraph graph;
        graph.start(judge.gauge());
        // カスタムゲージで遊んでいるときは、見せるゲージの列を遊んでいるゲージの値で上書きする
        const int graph_slot = judge.custom_play() >= 0 ? judge.display_gauge_type() : -1;
        if (graph_slot >= 0) graph.hp[graph_slot][0] = static_cast<int>(judge.play_hp());
        // リザルト画面（LR2: Scene05_Result.cpp）。タイマーはプレイ画面とは別
        SkinTimers rtimers;
        uint64_t result_start_ns = 0;
        int chart_op3 = 0, chart_op4 = 0;
        Lr2Skin* rskin = nullptr;
        auto enter_result = [&](bool quit_early) {
            // 鳴っている音は止めずにリザルトへ持ち込む（LR2 と同じ。閉店したときは、閉店した時点で止めてある）
            mixer.freeze_schedule();
            judge.gas_settle();  // GAS：曲の終わりのクリアの線で、ゲージを決める
            print_result(chart, judge);
            const PlayStats& ps = judge.stats();
            // LR2（Main.cpp:1697）：オートプレイ・1 ノーツも判定していない・GOOD 以上が 1 つも無いときは、
            // リザルトを出さず、記録もせずに選曲画面へ戻る
            ctx.quit_early = quit_early;
            // 開発用：BMJ_COURSE_STAGE_RESULT なら、オートでもコースの各ステージのリザルトを出す（記録はしない）
            // 開発用：BMJ_AUTO_RESULT なら、オートの単曲でもリザルトを出す（記録はしない）
            const bool dev_stage_result = opt.autoplay && ((course && dev_env("BMJ_COURSE_STAGE_RESULT")) || (!course && dev_env("BMJ_AUTO_RESULT")));
            if ((opt.autoplay && !dev_stage_result) || ps.note_current == 0 ||
                ps.judge_count[kGood] + ps.judge_count[kGreat] + ps.judge_count[kPGreat] == 0) {
                // コースでは、オートプレイなら次のステージへ進み、それ以外はコースをやめて選曲へ戻る
                if (course && !(opt.autoplay && !quit_early)) course->abandoned = true;
                done = true;
                return;
            }
            phase = Phase::Result;
            // 途中でやめたときと閉店したときはゲージを0にして、グラフの残りを埋める（LR2: Play.cpp:1659）
            std::array<double, kGaugeCount> hp{};
            for (int g = 0; g < kGaugeCount; ++g) hp[g] = quit_early || judge.failed() ? 0.0 : judge.gauge().hp(g);
            graph.finish(hp, ps.combo, ps.exscore(), ctx.best.exscore(), ctx.target.exscore());
            if (graph_slot >= 0) graph.overlay(graph_slot, quit_early || judge.failed() ? 0.0 : judge.play_hp());
            ctx.quit_early = quit_early;
            {
                // 演奏中に押されたままの入力をリザルトに持ち込まない
                std::vector<LaneInput> li;
                std::vector<ButtonInput> bi;
                input.collect(SDL_GetTicksNS() + 100000000ull, li, bi);
            }
            ctx.result_clear = (quit_early || judge.failed()) ? 1 : judge.clear();
            if (lamp_only && ctx.result_clear > 2) ctx.result_clear = 2;
            if (!scripts.empty()) {
                ModPlayResult mr;
                mr.clear = ctx.result_clear, mr.exscore = ps.exscore(), mr.max_combo = ps.max_combo;
                mr.bp = ps.bp() + ps.total_notes - ps.note_current, mr.total_notes = ps.total_notes;
                for (int k = 0; k < 6; ++k) mr.judge[k] = ps.judge_count[k];
                mr.judge[kPoor] -= ps.empty_poor, mr.judge[kEmptyPoor] = ps.empty_poor;  // POOR に数えた空POOR を分ける
                mr.fast = ps.fast, mr.slow = ps.slow, mr.quit_early = quit_early;
                static const char* gauge_names[kGaugeCount] = {"GROOVE", "HARD", "HAZARD", "EASY", "P-ATTACK", "G-ATTACK"};
                for (int g = 0; g < kGaugeCount; ++g) mr.gauges.push_back({gauge_names[g], judge.gauge().hp(g)});
                const auto keys = app.custom_gauges();
                for (int k = 0; k < judge.custom_count() && k < static_cast<int>(keys.size()); ++k)
                    mr.gauges.push_back({keys[k].first, judge.custom_hp(k)});
                scripts.play_end(mr);
                report_mod_errors();
            }
            // 比較に使う「このプレイの前の自己ベスト」は、記録する前に取っておく
            ctx.old_exscore = has_best ? best_score.exscore : 0;
            ctx.old_maxcombo = has_best ? best_score.max_combo : 0;
            ctx.old_minbp = has_best ? best_score.bp : -1;
            ctx.old_total_notes = has_best ? ps.total_notes : 0;
            const int64_t played_at =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const bool log_input = !recorded && !opt.autoplay && !opt.replay && started;
            if (!recorded && opt.save_score && !opt.autoplay && started && !(course && (lamp_only || slowed))) {
                PlayEntry entry;
                entry.played_at = played_at;
                entry.offset_count = offset_n;
                if (offset_n > 0) {
                    entry.offset_mean = offset_sum / offset_n;
                    entry.offset_sd = std::sqrt(std::max(0.0, offset_sq / offset_n - entry.offset_mean * entry.offset_mean));
                }
                if (log_input) entry.input_log = input_log_path(sha256, played_at);
                record_score(app, sha256, judge, ctx.result_clear, cfg, lamp_only, slowed, course != nullptr, score_ln_mode,
                             profile, ghost_log, chart_seed, entry);
                // LR2はリザルトの前に記録するので、リザルトで見える自己ベストのランプ（100-105）は更新後のもの
                // （コースのステージではフルコンボのときだけ上がる）
                if ((!course || ctx.result_clear >= 5) && !slowed) ctx.best_clear = std::max(ctx.best_clear, ctx.result_clear);
            }
            // 入力の記録（仕様書6章「リプレイ（入力ログ）」）。オートとリプレイの再生以外は、全部のプレーで 1 つずつ残す
            if (log_input) {
                record.sha256 = sha256;
                record.seed = chart_seed;
                record.random_version = kRandomAlgorithmVersion;
                record.judge_timing = cfg.judge_timing;
                record.played_at = played_at;
                record.mods = play_mod_key(app.active_mods());
                record.custom_gauge = cfg.custom_gauge;
                record.random = cfg.random, record.gauge = cfg.gauge, record.ln_mode = cfg.ln_mode, record.hs_fix = cfg.hs_fix;
                record.effect = cfg.effect, record.seven_key = cfg.seven_key, record.gas = cfg.gas, record.hi_speed = cfg.hi_speed;
                record.lane_cover = cfg.lane_cover, record.lane_cover_v = cfg.lane_cover_v;
                if (!save_replay(input_log_path(sha256, record.played_at), record)) std::fprintf(stderr, "入力の記録を保存できない\n");
            }
            // リプレイの保存（LR2: replaysave。Main.cpp:1796）。ALWAYS は毎回、HISCORE は EX が自己ベスト以上、
            // CLEAR はクリア、FULLCOMBO はフルコンボのとき（どれも EX が 0 なら残さない）。コースのステージは残さない
            if (!recorded && opt.save_score && !opt.autoplay && started && !course && cfg.replay_save > 0) {
                const int ex = ps.exscore();
                bool keep = cfg.replay_save == 1 || (ex > 0 && ((cfg.replay_save == 2 && ex >= ctx.old_exscore) ||
                                                              (cfg.replay_save == 3 && ctx.result_clear >= 2) ||
                                                              (cfg.replay_save == 4 && ctx.result_clear >= 5)));
                if (keep && !save_replay(replay_path(sha256), record)) std::fprintf(stderr, "リプレイを保存できない\n");
            }
            recorded = true;
            // クリア・失敗の音（LR2: ProcS_Result）
            mixer.play_system(ctx.result_clear < 2 ? kSysFail : kSysClear);
            // リザルトスキンは、結果が決まってから読む（#IF でクリア・失敗などを見るスキンがあるため）
            ctx.in_result = true;
            // リザルトのフリップ：プレイスキンの旗から始め、リザルトスキンの #FLIPRESULT / #DISABLEFLIP を読み込み順に当てる
            ctx.result_flip = skin && skin->flip_result;
            app.load_result_skin(ctx.load_options());
            rskin = app.result_skin.get();
            if (rskin) ctx.result_flip = rskin->flip_result;
            // リザルトのスキンのカスタマイズ（op 900〜）は、リザルトのスキンの値を見る
            if (rskin) ctx.skin = rskin;
            rtimers = SkinTimers();
            rtimers.set(0, 0);
            result_start_ns = SDL_GetTicksNS();
            if (rskin) {
                const auto& gc = ctx.result_flip ? rskin->gaugechart_2p : rskin->gaugechart;
                chart_op3 = gc[0].src.op[2];
                chart_op4 = gc[0].src.op[3];
                app.set_view(rskin->width, rskin->height, false);
            }
        };
        // リザルト画面での決定の入力（LR2: ProcI_Result）。押すたびに グラフ → ランク → 更新 → フェードアウト と進む
        auto result_input = [&] {
            double t = (SDL_GetTicksNS() - result_start_ns) / 1e6;
            if (!rskin) {
                done = true;
                return;
            }
            if (t <= rskin->startinput) return;
            if (!rtimers.on(151)) {
                rtimers.set(151, t);
                chart_op4 = static_cast<int>(t);  // グラフを最後まで描く
            } else if (rtimers.lapse(151, t) <= rskin->startinput_rank || rtimers.on(152)) {
                if (rtimers.lapse(152, t) > rskin->startinput_update && !rtimers.on(2)) rtimers.set(2, t);
            } else if (!opt.save_score || opt.autoplay || lamp_only || slowed) {
                // 記録しないプレイ（オートなど）とランプだけを残すプレイは、更新の表示を飛ばしてフェードアウトへ（LR2 と同じ）
                if (!rtimers.on(2)) rtimers.set(2, t);
            } else {
                rtimers.set(152, t);
            }
        };
        ctx.timers.set(0, 0);

        // 途中でやめるとき：タイマー2 を立てて、#FADEOUT のあいだ暗転してからリザルトへ（LR2 と同じ）
        bool fade_quit = false;
        // スコアとゲージの表示の補間（LR2: Play.cpp:1328）。スコアは 500ms、ゲージは変化 1% あたり 10ms かけて新しい値へ
        struct Tween {
            double from = 0, to = -1, t0 = 0, t1 = 0;
            double at(double t) const {
                if (to < 0) return 0;
                if (t >= t1 || t1 <= t0) return to;
                return from + (to - from) * (t - t0) / (t1 - t0);
            }
            void aim(double target, double t, double dur) {
                if (target == to) return;
                from = to < 0 ? target : at(t);
                to = target, t0 = t, t1 = t + dur;
            }
        } score_tw, hp_tw;
        auto quit_with_fade = [&] {
            mixer.freeze_schedule();  // フェードアウトの間は新しい音を鳴らさない（LR2: Play.cpp の演奏スレッドが止まる）
            fade_quit = judge.stats().note_current < judge.stats().total_notes;
            phase = Phase::Fadeout;
            ctx.timers.set(2, (SDL_GetTicksNS() - scene_start) / 1e6);
        };
        // ハイスピとカバー（仕様書6章）。見えている割合は、カバーと LIFT の分を引いたもの
        auto visible = [&] {
            double v = 1.0;
            if (cfg.lane_cover && cover_display) v -= cfg.lane_cover_v / 100.0;
            if (cfg.lift) v -= cfg.lift_v / 100.0;
            return std::max(0.01, v);
        };
        // フローティング：今のハイスピの、基準の BPM での緑数字。ハイスピを変えたら、それを次の曲にも持ち越す
        auto green_at_base = [&] { return green_number(lane_px, visible(), hs, fhs_bpm, cfg.base_speed); };
        auto keep_green = [&](double g) { hs = hs_for_green(lane_px, visible(), g, fhs_bpm, cfg.base_speed); };
        auto hs_changed = [&] {
            if (floating) cfg.fhs_green = static_cast<int>(std::lround(green_at_base()));
        };
        // カバーと LIFT を動かせるか。ギアチェンが「緑数字に戻す」のときは、曲が始まる前と最後のノーツの後だけ
        auto cover_movable = [&] { return !floating || cfg.gear_mode == 0 || !started || judge.finished(); };
        if (floating) keep_green(cfg.fhs_green);
        start_green = cfg.fhs_green;
        // ハイスピード固定のコースでは、ハイスピもカバーも変えられない
        const bool speed_locked = course && course->course->no_speed;
        // 演奏中の START / SELECT の操作（LR2: gameloop:163-327, 384-400）
        // START・SELECT の片方か、マウスの左ボタンを 250ms 以内に 2 回押したら、カバーを出し入れする（LR2: gameloop:385）
        auto cover_tap = [&](double now_ms) {
            if (cover_dbl_ms == 0 || now_ms - cover_dbl_ms > 250.0) {
                cover_dbl_ms = now_ms;
                return;
            }
            cover_dbl_ms = 0;
            if (floating && cfg.gear_mode == 1) {
                cfg.fhs_green = start_green;
                keep_green(start_green);
            } else {
                cover_display = !cover_display;
                if (floating) keep_green(cfg.fhs_green);
            }
        };
        // スキンの位置の調整で押している数字キー（1〜6 と 8・9。押していなければ 0）
        auto adjust_key = [] {
            const bool* ks = SDL_GetKeyboardState(nullptr);
            const SDL_Scancode keys[] = {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6};
            for (int i = 0; i < 6; ++i)
                if (ks[keys[i]]) return i + 1;
            return ks[SDL_SCANCODE_8] ? 8 : ks[SDL_SCANCODE_9] ? 9 : 0;
        };
        // スキンの位置の調整を 1 段階動かす（dx：右が +、dy：下が +）
        auto adjust_skin = [&](int dx, int dy) {
            SkinAdjust& a = cfg.skin_adjust[skin->path];
            switch (adjust_key()) {
            case 1: a.shift_x += dx * 10, a.shift_y += dy * 10; break;
            case 2: a.rate_x += dx * 5, a.rate_y += dy * 5; break;
            case 3: a.judge_x += dx * 10, a.judge_y += dy * 10; break;
            case 4: a.size_x += dx, a.size_y -= dy; break;
            case 5: a.dark = ((a.dark + (dy != 0 ? -dy : -dx)) % 3 + 3) % 3; break;
            case 6: a.note_x += dx * 10, a.note_y += dy * 10; break;
            case 9: a.note_fine_y += dy; break;  // 縦だけ
            case 8: a.judge_scale = std::clamp(a.judge_scale + (dx - dy) * 5, 10, 500); break;  // 右か上で大きく
            default: break;
            }
        };
        double scratch_analog_ms = -1e9, scratch_repeat_ms = 0, wheel_ms = -1e9;  // wheel_ms：最後にホイールが来た時刻  // 最後にアナログ皿が動いた時刻、キーの皿の繰り返し
        int scratch_repeat_dir = 0;
        auto play_buttons = [&](double now_ms) {
            bool held_start = input.button_held(kStart), held_select = input.button_held(kSelect);
            ctx.cover_adjusting = held_start || held_select;
            if (speed_locked) return;
            bool cover_active = cfg.lane_cover && cover_display;
            for (const ButtonInput& b : button_in) {
                if (!b.down) continue;
                if (b.button == kStart || b.button == kSelect) {
                    // 片方だけを押したときに、250ms 以内の2度押しでカバーを出し入れする。
                    // フローティングでは緑数字が変わらないようにハイスピを決め直す。ギアチェンが「緑数字に戻す」なら、
                    // カバーは動かさず、曲の始まりの緑数字に戻す
                    bool other = b.button == kStart ? held_select : held_start;
                    if (other) continue;
                    cover_tap(now_ms);
                    continue;
                }
                bool start_or_select = (held_start && b.button != kStart) || (held_select && b.button != kSelect);
                if (!start_or_select) continue;
                if (b.button == kScratchA || b.button == kScratchB) continue;  // 皿はこの下で、回した分だけ動かす
                if (b.button > kKey7) continue;
                int key = b.button - kKey1 + 1;
                if (floating) {
                    // フローティング：白鍵で 0.50 下げ、黒鍵で 0.50 上げる（今の値から足し引きし、端で止まる）
                    hs = std::clamp(hs + (key % 2 ? -50 : 50), kFloatingHsMin, kFloatingHsMax);
                    hs_changed();
                    continue;
                }
                // クラシック：1・3・5 で遅く、2・4 で速く、6・7 でカバー（カバーが無ければハイスピード）
                if (key == 1 || key == 3 || key == 5) cfg.hi_speed -= cfg.hs_margin;
                else if (key == 2 || key == 4) cfg.hi_speed += cfg.hs_margin;
                else if (key == 6) cover_active ? cfg.lane_cover_v -= cfg.shutter_margin : cfg.hi_speed += cfg.hs_margin;
                else if (key == 7) cover_active ? cfg.lane_cover_v += cfg.shutter_margin : cfg.hi_speed -= cfg.hs_margin;
            }
            // START か SELECT を押しながら皿（時計回りで + ）。アナログ皿は回した分（軸の 1 目盛りで 1）、
            // キーやボタンの皿は、回すたび（押すたび）に 1。続けて何度回しても 1 回ずつ数える。
            // 0.6 秒以上回し続けたときだけ、そこから 0.1 秒ごとに 1 ずつ動かす（ちょんと回しても 0.35 秒ほど
            // 押されたままになるコントローラーがあるので、それより短い押し方では繰り返さない）。
            // SUDDEN+ が出ていなくて LIFT を使っていれば LIFT（OpenLR2 の拡張。2 度押しで SUDDEN+ をしまえば動かせる）。
            // フローティングで SUDDEN+ が出ていれば SUDDEN+（緑数字が変わらないようにハイスピを決め直す。ギアチェン）。
            // どちらでもなければ、フローティングはハイスピの微調整（0.01）。
            // クラシックの SUDDEN+ は LR2 と同じく皿では動かさない（START＋6・7 とホイール）。
            // 皿を回すとホイールの信号も出すコントローラーでは、ホイールの側で動かすので、皿の分は足さない（二重に動く）
            const int ticks = input.take_analog_ticks();
            if (ticks != 0) scratch_analog_ms = now_ms;
            int steps = 0;
            if (held_start || held_select) {
                steps = ticks;
                const int dir = input.button_held(kScratchB) ? 1 : input.button_held(kScratchA) ? -1 : 0;
                if (now_ms - scratch_analog_ms > 200 && dir != 0) {
                    if (scratch_repeat_dir != dir) {
                        steps += dir;
                        scratch_repeat_ms = now_ms + 500;
                    } else if (now_ms - scratch_repeat_ms >= 100) {
                        scratch_repeat_ms = now_ms, steps += dir;
                    }
                    scratch_repeat_dir = dir;
                } else if (dir == 0) {
                    scratch_repeat_dir = 0;
                }
            } else {
                scratch_repeat_dir = 0;
            }
            if (now_ms - wheel_ms < 500) steps = 0;
            if (steps != 0) {
                if (dev_env("BMJ_DEBUG_INPUT")) std::printf("カバー・LIFT を %+d（%.1f ms）\n", steps, now_ms), std::fflush(stdout);
                if (cover_active) {
                    if (floating && cover_movable()) {
                        cfg.lane_cover_v = std::clamp(cfg.lane_cover_v + steps, 0, 100);
                        keep_green(cfg.fhs_green);
                    }
                } else if (cfg.lift && cover_movable()) {
                    cfg.lift_v = std::clamp(cfg.lift_v - steps, 0, 100);  // LIFT は皿の向きと逆に動かす（時計回りで下がる）
                    if (floating) keep_green(cfg.fhs_green);
                } else if (floating) {
                    hs = std::clamp(hs + steps, kFloatingHsMin, kFloatingHsMax);
                    hs_changed();
                }
            }
            cfg.lane_cover_v = std::clamp(cfg.lane_cover_v, 0, 100);
            if (!floating) cfg.hi_speed = std::clamp(cfg.hi_speed, cfg.hs_min, cfg.hs_max);
            ctx.cover_adjusting = held_start || held_select;
        };

        // スライダー 4（レーンカバー）と 27（LIFT）をマウスでつかんで動かす（LR2: SliderByTime）
        const SkinObject* dragging = nullptr;
        auto slider_value = [&](const SkinObject& o, float mx, float my) {
            SkinDraw d = eval_dst(o.dst, ctx.timers.lapse(o.dst.timer, (SDL_GetTicksNS() - scene_start) / 1e6));
            float len = static_cast<float>(std::max(1, o.src.op[1]));
            float r = o.src.op[0] == 0 ? (d.y + d.h / 2 - my) / len : o.src.op[0] == 1 ? (mx - d.x - d.w / 2) / len
                      : o.src.op[0] == 2 ? (my - d.y - d.h / 2) / len : (d.x + d.w / 2 - mx) / len;
            return static_cast<int>(std::lround(std::clamp(r, 0.0f, 1.0f) * 100));
        };
        auto drag_to = [&](float mx, float my) {
            int v = slider_value(*dragging, mx, my);
            if (dragging->src.op[2] == 27) cfg.lift_v = v;
            else cfg.lane_cover_v = v;
            if (floating) keep_green(cfg.fhs_green);
        };
        while (!done) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    SDL_ConvertEventToRenderCoordinates(app.renderer, &e);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && skin && phase == Phase::Play &&
                    !speed_locked && cover_movable()) {
                    for (const SkinObject& o : skin->sliders) {
                        int ty = o.src.op[2];
                        bool usable = (ty == 4 && cfg.lane_cover) || (ty == 27 && cfg.lift);
                        if (!usable || o.src.op[3] == 1 || o.dst.empty()) continue;
                        SkinDraw d = eval_dst(o.dst, ctx.timers.lapse(o.dst.timer, (SDL_GetTicksNS() - scene_start) / 1e6));
                        if (!d.visible) continue;
                        float len = static_cast<float>(o.src.op[1]);
                        SDL_FRect rc{d.x, d.y, d.w, d.h};
                        if (o.src.op[0] == 0) rc.y -= len, rc.h += len;
                        else if (o.src.op[0] == 1) rc.w += len;
                        else if (o.src.op[0] == 2) rc.h += len;
                        else rc.x -= len, rc.w += len;
                        if (e.button.x >= rc.x && e.button.x < rc.x + rc.w && e.button.y >= rc.y && e.button.y < rc.y + rc.h) {
                            dragging = &o;
                            drag_to(e.button.x, e.button.y);
                            break;
                        }
                    }
                }
                // マウスの左ボタンの 2 度押しでもカバーを出し入れする（スライダーをつかんだときは除く）
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && phase == Phase::Play && !speed_locked &&
                    !dragging)
                    cover_tap((SDL_GetTicksNS() - scene_start) / 1e6);
                if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) drag_to(e.motion.x, e.motion.y);
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) dragging = nullptr;
                // 演奏中の右クリックで曲を終える（LR2 と同じ。設定で止められる）
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT && phase == Phase::Play &&
                    !cfg.no_right_click_exit)
                    quit_with_fade();
                switch (e.type) {
                case SDL_EVENT_QUIT: quit = true, done = true, exit = PlayExit::Quit; break;
                case SDL_EVENT_JOYSTICK_ADDED: SDL_OpenJoystick(e.jdevice.which); break;
                case SDL_EVENT_MOUSE_WHEEL: {
                    // ホイールで、カバーが出ていなくて LIFT を使っていれば LIFT、それ以外はレーンカバー（LR2: gameloop:183。
                    // LIFT は OpenLR2 の拡張）。皿を回すとホイールの信号も出すコントローラーがあり、LR2 で「皿でカバーが
                    // 動く」のはこれ。LIFT はカバーと逆向きに動かす
                    const int w = static_cast<int>(std::lround(e.wheel.y));
                    wheel_ms = (SDL_GetTicksNS() - scene_start) / 1e6;
                    if (dev_env("BMJ_DEBUG_INPUT")) std::printf("ホイール %+d（%.1f ms）\n", w, wheel_ms), std::fflush(stdout);
                    if (adjust_key() > 0 && skin) {
                        // 数字キーを押しながらのホイールは、矢印の上下と同じ（BMjoy の拡張。矢印が押しにくいキーボード向け）。
                        // Shift を押していれば左右
                        const int steps = w != 0 ? w : static_cast<int>(std::lround(e.wheel.x));
                        const bool side = w == 0 || (SDL_GetModState() & SDL_KMOD_SHIFT);
                        for (int i = 0; i < std::abs(steps); ++i) {
                            const int d = steps > 0 ? -1 : 1;  // 上へ回すと ↑
                            if (side) adjust_skin(-d, 0);      // 上へ回すと →
                            else adjust_skin(0, d);
                        }
                        break;
                    }
                    if (phase != Phase::Play || !cover_movable() || speed_locked || w == 0) break;
                    const bool cover_shown = cfg.lane_cover && cover_display;
                    if (cfg.lift && !cover_shown) {
                        cfg.lift_v = std::clamp(cfg.lift_v + w, 0, 100);
                        if (floating) keep_green(cfg.fhs_green);
                    } else if (cfg.lane_cover == 1) {
                        cfg.lane_cover_v = std::clamp(cfg.lane_cover_v - w, 0, 100);
                        if (floating) keep_green(cfg.fhs_green);
                    }
                    break;
                }
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.repeat) break;
                    if (e.key.key == SDLK_ESCAPE) {
                        if (phase == Phase::Result) result_input();
                        else if (phase == Phase::Play) quit_with_fade();
                        // フェードアウト中と閉店中は効かない（LR2: 終わりの操作は演奏中だけ）
                    } else if (e.key.key == SDLK_RETURN && phase == Phase::Result) {
                        result_input();
                    } else if (e.key.key == SDLK_F5 && phase == Phase::Result && !course && !opt.replay) {
                        retry = true, done = true;
                    } else if (e.key.key == SDLK_F4) {
                        app.toggle_fullscreen();
                    } else if ((e.key.key == SDLK_UP || e.key.key == SDLK_DOWN || e.key.key == SDLK_LEFT || e.key.key == SDLK_RIGHT) &&
                               adjust_key() > 0 && skin) {
                        // スキンの位置の調整（LR2: gameloop:426）。数字キーを押している間は、矢印でその項目を変えてスキンごとに残す
                        adjust_skin(e.key.key == SDLK_RIGHT ? 1 : e.key.key == SDLK_LEFT ? -1 : 0,
                                    e.key.key == SDLK_DOWN ? 1 : e.key.key == SDLK_UP ? -1 : 0);
                    } else if ((e.key.key == SDLK_RIGHT || e.key.key == SDLK_LEFT) && (e.key.mod & SDL_KMOD_SHIFT)) {
                        // 判定タイミング（BMjoy の拡張。LR2 はオプション画面でしか変えられない）
                        int d = e.key.key == SDLK_RIGHT ? 1 : -1;
                        cfg.judge_timing = std::clamp(cfg.judge_timing + d, -199, 199);
                    } else if ((e.key.key == SDLK_UP || e.key.key == SDLK_DOWN) && !speed_locked) {
                        // ハイスピード（LR2: gameloop:477。端まで行くと反対の端へ回る）。フローティングは 0.50 ずつで、端で止まる
                        if (floating) {
                            hs = std::clamp(hs + (e.key.key == SDLK_UP ? 50 : -50), kFloatingHsMin, kFloatingHsMax);
                            hs_changed();
                        } else {
                            cfg.hi_speed += e.key.key == SDLK_UP ? cfg.hs_margin : -cfg.hs_margin;
                            if (cfg.hi_speed > cfg.hs_max) cfg.hi_speed = cfg.hs_min;
                            else if (cfg.hi_speed < cfg.hs_min) cfg.hi_speed = cfg.hs_max;
                        }
                    } else if ((e.key.key == SDLK_RIGHT || e.key.key == SDLK_LEFT) && !speed_locked) {
                        // レーンカバーの量（LR2: gameloop:481）
                        int d = e.key.key == SDLK_RIGHT ? cfg.shutter_margin : -cfg.shutter_margin;
                        if (cover_movable()) {
                            cfg.lane_cover_v = std::clamp(cfg.lane_cover_v + d, 0, 100);
                            if (floating) keep_green(cfg.fhs_green);
                        }
                    }
                    break;
                default: break;
                }
                input.handle(e);
            }
            if (phase == Phase::Result) {
                // 鍵盤でも進める。白鍵と黒鍵を同時に押していたら、フェードアウトのあとで同じ譜面をもう一度（LR2のクイックリトライ）
                std::vector<LaneInput> li;
                std::vector<ButtonInput> bi;
                input.collect(SDL_GetTicksNS(), li, bi);
                for (const ButtonInput& b : bi)
                    if (b.down && b.button <= kKey7) result_input();
                double t = (SDL_GetTicksNS() - result_start_ns) / 1e6;
                if (rskin) {
                    if (t > chart_op3 && !rtimers.on(150)) rtimers.set(150, t);
                    if (t > chart_op4 && !rtimers.on(151)) rtimers.set(151, t);
                }
                // リザルトを抜けるフェードアウトの間に、音も #FADEOUT の時間で絞る（LR2: Scene05_Result.cpp:286）
                if (rskin && rtimers.on(2) && !audio_fading) mixer.fade_all(0.0f, rskin->fadeout), audio_fading = true;
                if (!rskin || (rtimers.on(2) && rtimers.lapse(2, t) >= rskin->fadeout)) {
                    if (rskin && rtimers.on(2)) {
                        bool white = input.button_held(kKey1) || input.button_held(kKey1 + 2) || input.button_held(kKey1 + 4) ||
                                     input.button_held(kKey1 + 6);
                        bool black = input.button_held(kKey1 + 1) || input.button_held(kKey1 + 3) || input.button_held(kKey1 + 5);
                        if (white && black && !course && !opt.replay) retry = true;
                        // 開発用：BMJ_RETRY_ONCE があれば、1 回目のリザルトを抜けるときにクイックリトライしたことにする
                        if (!retried && dev_env("BMJ_RETRY_ONCE") && !course && !opt.replay) retry = true;
                        done = true;
                    }
                }
            }

            joystick_init().open_new();
            uint64_t now_ns = SDL_GetTicksNS();
            double now_ms = (now_ns - scene_start) / 1e6;
            double dt_ms = (now_ns - prev_ns) / 1e6;
            prev_ns = now_ns;
            ctx.now_ms = now_ms;
            if (phase != Phase::Result) {
                const PlayStats& ps0 = judge.stats();
                double score = ps0.total_notes > 0
                                   ? (ps0.judge_count[kGood] + (ps0.judge_count[kGreat] + ps0.judge_count[kPGreat] * 2) * 2) * 50000.0 / ps0.total_notes
                                   : 0;
                score_tw.aim(static_cast<int>(score), now_ms, 500);
                double hp = judge.play_hp();
                hp_tw.aim(hp, now_ms, std::abs(hp_tw.at(now_ms) - hp) * 10);
                ctx.score_print = score_tw.at(now_ms);
                ctx.hp_print = hp_tw.at(now_ms);
            } else {
                ctx.score_print = ctx.hp_print = -1;
            }
            ctx.hi_speed = hs;
            {
                // 緑数字（いまの BPM での値）と、見えている時間（ms）
                double bpm = chart.bpm_at(started ? mixer.song_time_us_at(now_ns) : 0) * speed_multiplier;
                if (constant) bpm = 150.0;
                double g = green_number(lane_px, visible(), hs, bpm, cfg.base_speed);
                ctx.green_number = static_cast<int>(std::lround(g));
                ctx.visible_ms = static_cast<int>(std::lround(g / 0.6));
                ctx.lift = cfg.lift ? cfg.lift_v : 0;
            }
            ctx.judge_timing = cfg.judge_timing;
            ctx.lane_cover = cfg.lane_cover_v;
            ctx.lane_cover_on = cfg.lane_cover && cover_display;
            ctx.loaded = load_done.load();
            if (loaded && !loader_joined) {
                loader.join();
                loader_joined = true;
                std::printf("曲の始まり：キー音と BGA を読み終えた %.0f ms\n", (SDL_GetTicksNS() - prelude_ns) / 1e6);
                for (const auto& er : load_errors) std::printf("%s\n", er.c_str());
                // GPUへのアップロードはメインスレッドで行う（仕様書4章）
                bga_textures.assign(bga_images.size(), nullptr);
                for (size_t i = 0; i < bga_images.size(); ++i) {
                    if (!bga_images[i].rgba.empty()) bga_textures[i] = app.skin_renderer->create_texture(bga_images[i]);
                    if (videos[i]) {
                        // 動画は毎フレーム絵を書き換えるテクスチャ。最初は黒
                        SDL_Texture* t = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                           videos[i]->width(), videos[i]->height());
                        if (t) {
                            std::vector<uint8_t> blank(static_cast<size_t>(videos[i]->width()) * videos[i]->height() * 4, 0);
                            for (size_t k = 3; k < blank.size(); k += 4) blank[k] = 255;
                            SDL_UpdateTexture(t, nullptr, blank.data(), videos[i]->width() * 4);
                        }
                        bga_textures[i] = t;
                    }
                }
                bga_images.clear();
                if (!stage_image.rgba.empty()) stage_tex = app.skin_renderer->create_texture(stage_image);
                if (!back_image.rgba.empty()) back_tex = app.skin_renderer->create_texture(back_image);
                ctx.has_stagefile = stage_tex != nullptr;
                ctx.has_backbmp = back_tex != nullptr;
                for (SkinRenderer* sr : {app.skin_renderer.get(), app.result_renderer.get()}) {
                    sr->set_image_override(100, stage_tex);
                    sr->set_image_override(101, back_tex);
                }
            }

            // 場面の進行（LR2: Scene04_Play.cpp:1748-1768）
            int startinput = skin ? skin->startinput : 0;
            int load_wait = skin ? skin->loadstart + skin->loadend : 0;
            int playstart = skin ? skin->playstart : 1000;
            if (!ctx.timers.on(1) && now_ms >= startinput) ctx.timers.set(1, now_ms), ctx.timers.set(11, now_ms);  // LR2: 入力受付と同時に 11 も
            if (!ctx.timers.on(40) && loader_joined && now_ms >= load_wait) ctx.timers.set(40, now_ms);
            // 曲は #PLAYSTART が過ぎ、START・SELECT と数字キー（1〜8・0）が離されてから始める（LR2: Play.cpp:1755）
            auto start_blocked = [&] {
                if (input.button_held(kStart) || input.button_held(kSelect)) return true;
                const bool* ks = SDL_GetKeyboardState(nullptr);
                for (SDL_Scancode sc : {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
                                        SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0})
                    if (ks[sc]) return true;
                return false;
            };
            if (ctx.timers.on(40) && !started && ctx.timers.lapse(40, now_ms) >= playstart && !start_blocked()) {
                std::vector<ScheduledSound> schedule;
                uint32_t rate = mixer.sample_rate();
                auto frame_of = [&](int64_t us) { return us * static_cast<int64_t>(rate) / 1000000; };
                for (const BgmEvent& b : chart.bgm) schedule.push_back({frame_of(b.time_us), b.sound});
                for (const Chart::Beat& b : metro_beats)
                    schedule.push_back({frame_of(b.time_us), static_cast<uint16_t>(b.head ? metro_accent : metro_beat)});
                if (opt.autoplay)
                    for (const Note& n : chart.notes)
                        if ((n.kind == NoteKind::Normal || n.kind == NoteKind::Long) && n.sound)
                            schedule.push_back({frame_of(n.time_us), n.sound, true});
                mixer.set_song(&bank.samples(), std::move(schedule));
                mixer.stop_system(kSysDecide);
                mixer.start(0);
                started = true;
                std::printf("曲の始まり：音を鳴らし始めた %.0f ms\n", (SDL_GetTicksNS() - prelude_ns) / 1e6);
            }

            int64_t song_us = started ? mixer.song_time_us_at(now_ns) : 0;
            double song_ms = song_us / 1000.0;
            // 曲の時刻 → シーンの時刻（タイマーは曲の出来事の瞬間に合わせて立てる）
            auto scene_ms_of = [&](int64_t t_us) { return now_ms - (song_ms - t_us / 1000.0); };

            if (!started && phase == Phase::Play && ctx.timers.on(1)) {
                // 曲が始まる前も、入力受付（タイマー1）から先は鍵盤を光らせる（LR2: LR2_gameloop.cpp:120）。判定はしない
                lane_in.clear();
                button_in.clear();
                input.collect(now_ns, lane_in, button_in);
                play_buttons(now_ms);
                for (const LaneInput& li : lane_in) {
                    double at = now_ms - (now_ns - li.time_ns) / 1e6;
                    if (li.down) ctx.timers.set(100 + li.lane, at), ctx.timers.reset(120 + li.lane);
                    else ctx.timers.reset(100 + li.lane), ctx.timers.set(120 + li.lane, at);
                }
            }
            if (started && phase == Phase::Play) {
                ctx.song_ms = song_ms;
                ctx.timers.set(41, now_ms - song_ms);
                ctx.timers.set(142, now_ms - song_ms);
                lane_in.clear();
                button_in.clear();
                input.collect(now_ns, lane_in, button_in);
                play_buttons(now_ms);
                if (feed_input) {
                    // 開発用：ファイルの入力を手で押したものとして渡す
                    for (; replay_pos < replay.events.size() && replay.events[replay_pos].time_us <= song_us - 5000; ++replay_pos) {
                        const ReplayEvent& ev = replay.events[replay_pos];
                        LaneInput li;
                        li.lane = ev.lane, li.down = ev.down;
                        li.time_ns = now_ns - static_cast<uint64_t>(std::max<int64_t>(0, song_us - ev.time_us)) * 1000;
                        lane_in.push_back(li);
                    }
                }
                if (opt.replay) {
                    // リプレイ：記録した入力を、曲の時刻が来たものから判定に渡す（手元の鍵盤は判定に使わない）
                    for (; replay_pos < replay.events.size() && replay.events[replay_pos].time_us <= song_us - 5000; ++replay_pos) {
                        const ReplayEvent& ev = replay.events[replay_pos];
                        if (ev.down) judge.press(ev.lane, ev.time_us);
                        else judge.release(ev.lane, ev.time_us);
                        double at = scene_ms_of(ev.time_us);
                        if (ev.down) ctx.timers.set(100 + ev.lane, at), ctx.timers.reset(120 + ev.lane);
                        else ctx.timers.reset(100 + ev.lane), ctx.timers.set(120 + ev.lane, at);
                    }
                    lane_in.clear();
                }
                for (const LaneInput& li : lane_in) {
                    int64_t t = mixer.song_time_us_at(li.time_ns);
                    if (li.down) judge.press(li.lane, t);
                    else judge.release(li.lane, t);
                    if (!opt.autoplay) record.events.push_back({t, static_cast<uint8_t>(li.lane), li.down});
                    // 鍵盤の光（LR2: LR2_gameloop.cpp:120）
                    double at = now_ms - (now_ns - li.time_ns) / 1e6;
                    if (li.down) ctx.timers.set(100 + li.lane, at), ctx.timers.reset(120 + li.lane);
                    else ctx.timers.reset(100 + li.lane), ctx.timers.set(120 + li.lane, at);
                }
                // 入力が遅れて届く分の余裕を見て、少し手前の時刻まで進める
                judge.advance(song_us - 5000);
                for (uint16_t s : judge.sounds_to_stop()) mixer.stop_now(s);
                for (uint16_t s : judge.sounds_to_play()) {
                    if (s == kMineSystemSound) mixer.play_system(kSysMine);
                    else mixer.play_now(s);
                }
                judge.sounds_to_stop().clear();
                judge.sounds_to_play().clear();
                for (const JudgeEvent& je : judge.events()) {
                    if (!scripts.empty()) {
                        ModJudgeInfo mi;
                        mi.time_ms = je.time_us / 1000.0, mi.lane = je.lane;
                        mi.judge = je.judge >= 100 ? je.judge - 100 : je.judge;
                        mi.has_offset = je.has_offset, mi.offset_ms = je.offset_ms, mi.fast = je.fast;
                        mi.mine = je.mine, mi.ln_end = je.ln_end;
                        mi.combo = judge.stats().combo, mi.exscore = judge.stats().exscore(), mi.hp = judge.play_hp();
                        scripts.judge(mi);
                    }
                    if (je.mine) {
                        // 地雷：前の判定の絵をもう一度出す
                        ctx.timers.set(46, scene_ms_of(je.time_us));
                        continue;
                    }
                    if (cfg.auto_adjust > 0 && !opt.autoplay && !opt.replay && je.has_offset && !je.ln_end && je.judge >= kBad && je.judge <= kPGreat) {
                        adjust_sum += je.offset_ms;
                        if (++adjust_count > 9) {
                            if (adjust_sum > 0) cfg.judge_timing = std::min(199, cfg.judge_timing + 1);
                            else if (adjust_sum < 0) cfg.judge_timing = std::max(-199, cfg.judge_timing - 1);
                            adjust_count = adjust_sum = 0;
                        }
                    }
                    double at = scene_ms_of(je.time_us);
                    int j = je.judge >= 100 ? je.judge - 100 : je.judge;
                    if (je.judge >= 100) {
                        // LNの始点（LR2: ProcLongnote。PG/GR/GDでタイマー70）
                        if (j >= kGood) ctx.timers.set(70 + je.lane, at);
                    } else {
                        // CN・HCN の始点は、ここで数えるのと同時に、押さえている間の演出（タイマー70）も始める
                        if (je.ln_start && j >= kGood) ctx.timers.set(70 + je.lane, at);
                        if (j >= kGreat && !je.ln_release) ctx.timers.set(50 + je.lane, at);  // ボム（LN を手前で離したときは出さない）
                        if (j >= kPoor) ctx.on_judged();
                        if (j >= kEmptyPoor && j <= kPGreat) ghost_log.push_back(static_cast<int8_t>(j));
                        // OpenLR2の拡張：直前の FAST/SLOW とずれ（ms）
                        if (j == kPGreat) ctx.last_fast_slow = 0;
                        else if (j == kEmptyPoor) ctx.last_fast_slow = 1;
                        else if (je.has_offset) ctx.last_fast_slow = je.fast ? 1 : 2;
                        else ctx.last_fast_slow = 2;
                        if (je.has_offset) {
                            ctx.last_offset_ms = je.offset_ms;
                            ++offset_n, offset_sum += je.offset_ms, offset_sq += static_cast<double>(je.offset_ms) * je.offset_ms;
                        }
                        if (j < kGood) ctx.last_miss_ms = at, last_miss_ms = at;
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
                // BGAの切り替え（LR2: Play.cpp:1404-1470）
                if (!bga_poor_init && !bga_textures.empty()) {
                    bga_poor = bga_textures[0] ? 0 : -1;
                    bga_poor_init = true;
                }
                while (bga_next < chart.bga.size() && chart.bga[bga_next].time_us < song_us) {
                    const BgaEvent& be = chart.bga[bga_next++];
                    if (be.layer == BgaEvent::Layer::Base) bga_base = be.image;
                    else if (be.layer == BgaEvent::Layer::Layer) bga_layer = be.image;
                    else bga_poor = be.image;
                    // 動画は、BGA に出るたびに頭から再生する（LR2: Play.cpp:1409。繰り返しはしない）
                    if (be.layer != BgaEvent::Layer::Poor && be.image < videos.size() && videos[be.image]) {
                        if (video_start[be.image] >= 0) videos[be.image]->restart();
                        video_start[be.image] = be.time_us / 1000.0;
                    }
                }
                for (size_t i = 0; i < videos.size(); ++i) {
                    if (!videos[i] || video_start[i] < 0 || i >= bga_textures.size() || !bga_textures[i]) continue;
                    VideoDecoder::Frame f;
                    if (videos[i]->take(song_ms - video_start[i], f) && !f.rgba.empty())
                        SDL_UpdateTexture(bga_textures[i], nullptr, f.rgba.data(), f.w * 4);
                }
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
                graph.log(static_cast<int>(song_ms), static_cast<int>(chart.end_us / 1000), judge.gauge(), ps.combo,
                          ps.exscore(), ctx.best.exscore(), ctx.target.exscore());
                if (graph_slot >= 0) graph.overlay(graph_slot, judge.play_hp());
                if (!last_note_set && ps.note_current >= ps.total_notes) ctx.timers.set(143, now_ms), last_note_set = true;
                if (!fc_set && ps.total_notes > 0 && ps.combo == ps.total_notes) ctx.timers.set(48, now_ms), fc_set = true;
                // ゲージのタイマー（LR2: ApplyJudgeNote の 42/44）
                int hp = static_cast<int>(judge.play_hp()) / 2 * 2;
                // 初期値が 100% のゲージ（HARD 系）は、曲が始まった時点で MAX（LR2: タイマー41 と同時に 44）
                if (old_hp < 0 && hp == 100) ctx.timers.set(44, now_ms);
                if (old_hp >= 0 && hp != old_hp) {
                    if (old_hp <= 0) hp = 0, ctx.timers.reset(44);
                    else if (hp != 100) ctx.timers.reset(44);
                    if (old_hp < hp) {
                        if (hp == 100) ctx.timers.reset(42), ctx.timers.set(44, now_ms);
                        else ctx.timers.set(42, now_ms);
                    }
                }
                old_hp = hp;

                // コースでは GROOVE/EASY も 2% を割ると閉店する
                bool survival = course || !(judge.display_gauge_type() == kGroove || judge.display_gauge_type() == kEasy);
                if (survival && judge.failed()) {
                    phase = Phase::Failed;
                    ctx.timers.set(3, now_ms);
                    mixer.stop();
                    mixer.play_system(kSysStop);  // 閉店（LR2: Play.cpp:1367）
                    view.failed = true;
                } else if (song_us > chart.end_us) {
                    mixer.freeze_schedule();
                    phase = Phase::Fadeout;
                    ctx.timers.set(2, now_ms);
                }
            }
            // START と SELECT を同時に押してもやめる。全部のノーツを処理したあとは、どちらか片方でも終わる（LR2: Play.cpp:1197）
            if (phase == Phase::Play) {
                const PlayStats& s = judge.stats();
                bool start = input.button_held(kStart), select = input.button_held(kSelect);
                if ((start && select) || (s.note_current >= s.total_notes && (start || select))) quit_with_fade();
            }
            if (phase == Phase::Fadeout && ctx.timers.lapse(2, now_ms) >= (skin ? skin->fadeout : 0)) {
                enter_result(fade_quit);
            } else if (phase == Phase::Failed && ctx.timers.lapse(3, now_ms) >= (skin ? skin->close : 2050)) {
                enter_result(false);
            }

            if (dev_env("BMJ_DEBUG_LOOP")) {
                // 開発用：1秒あたりの入力処理の回数、描画の回数、描画1回にかかった時間、フレームの間隔の最大
                static uint64_t t = now_ns, last_frame = now_ns;
                static int loops = 0;
                static double draw_ms_sum = 0, draw_ms_max = 0, gap_max = 0;
                static int draws = 0;
                ++loops;
                if (frame_ns == 0 || now_ns >= next_frame_ns) {
                    gap_max = std::max(gap_max, (now_ns - last_frame) / 1e6);
                    last_frame = now_ns;
                }
                if (app.last_draw_ms >= 0) {
                    draw_ms_sum += app.last_draw_ms;
                    draw_ms_max = std::max(draw_ms_max, app.last_draw_ms);
                    ++draws;
                    app.last_draw_ms = -1;
                }
                if (now_ns - t > 1000000000ull) {
                    std::printf("入力処理 %d回/秒、描画 %dfps、描画1回 平均%.2fms 最大%.2fms、フレーム間隔の最大 %.1fms\n", loops, ctx.fps,
                                draws ? draw_ms_sum / draws : 0.0, draw_ms_max, gap_max);
                    loops = 0, draws = 0, draw_ms_sum = 0, draw_ms_max = 0, gap_max = 0;
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

            // 皿の回転（LR2: LR2_gameloop.cpp:349）。押している向きに 720°/秒。
            // 離しているときは、曲が始まってから（タイマー41）だけ 360°/秒でゆっくり回る
            if (input.button_held(kScratchA)) scratch_angle -= static_cast<float>(dt_ms * 0.72);
            else if (input.button_held(kScratchB)) scratch_angle += static_cast<float>(dt_ms * 0.72);
            else if (started) scratch_angle += static_cast<float>(dt_ms * 0.36);
            scratch_angle = std::fmod(scratch_angle, 360.0f);

            SDL_Renderer* renderer = app.renderer;
            double render_beat = chart.pos_at(song_us + cfg.judge_timing * 1000);
            if (phase == Phase::Result && rskin) {
                double t = (now_ns - result_start_ns) / 1e6;
                PlaySkinState st;
                st.now_ms = t;
                st.timers = &rtimers;
                st.option = [&](int o) { return ctx.option(o); };
                st.number = [&](int id) { return ctx.number(id); };
                st.bargraph = [&](int ty, double& v, double& m) { return ctx.bargraph(ty, v, m); };
                st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx.slider(ty, v, lo, hi); };
                st.text = [&](int id) { return ctx.text(id); };
                st.button = [&](int ty) { return ctx.button(ty); };
                st.chart = &chart;
                st.judge = &judge;
                st.graph = &graph;
                st.graph_gauge = judge.display_gauge_type();
                st.gauge_survival = course || !(judge.display_gauge_type() == kGroove || judge.display_gauge_type() == kEasy);
                set_gauge_tint(st, judge, app);
                // ゲージの欄：カスタムゲージならスキンに足したそのコマ（白い字）を、ゲージの色に染めて出す。
                // 今回のクリア（ボタン 901）：付いたランプの色に染める（カスタムランプ、または色を変えた本体のランプ）
                const int cg = course ? -1 : judge.custom_play();
                st.button_extra_frame = [&judge, cg](int ty) { return ty == 40 && cg >= 0 ? judge.custom_def(cg).frame : -1; };
                uint32_t lamp_tint = 0;
                const int clear = judge.clear();
                if (cg >= 0 && clear >= 2 && clear < 5 && judge.custom_cleared(cg)) {
                    const auto lamps = app.custom_lamps();
                    if (auto it = lamps.find(judge.custom_def(cg).lamp_key); it != lamps.end()) lamp_tint = 0xff000000u | it->second->color_at(t);
                }
                if (!lamp_tint && clear >= 2) lamp_tint = app.standard_lamp_tint(clear, t);
                st.button_tint = [&st, lamp_tint](int ty, bool extra) -> uint32_t {
                    if (ty == 40 && extra) return st.gauge_tint;
                    if (ty == kClearLabelButton) return lamp_tint;
                    return 0;
                };
                st.total_notes = judge.stats().total_notes;
                st.chart_op3 = chart_op3;
                st.chart_op4 = chart_op4;
                st.result_flip = ctx.result_flip;
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                app.result_renderer->draw_result(*rskin, st);
            } else if (phase == Phase::Result) {
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
                st.button = [&](int t) { return ctx.button(t); };
                st.chart = &chart;
                st.judge = &judge;
                st.notes_visible = started;
                st.render_beat = render_beat;
                st.hi_speed = hs;
                st.speed_multiplier = scroll_multiplier;
                st.lift_px = cfg.lift ? static_cast<float>(lane_px * cfg.lift_v / 100.0) : 0.0f;
                if (auto it = cfg.skin_adjust.find(skin->path); it != cfg.skin_adjust.end()) st.adjust = it->second;
                st.base_speed = cfg.base_speed;
                st.effect = cfg.effect;
                st.autoplay = opt.autoplay;
                st.judge_draw = ctx.judge_draw;
                st.show_judge = !silent;
                // 判定の絵の横のコンボは、コースならコース通し（LR2: Play.cpp:237）
                st.combo_draw = course ? judge.stats().course_combo : judge.stats().combo;
                st.gauge_hp = ctx.hp_print >= 0 ? ctx.hp_print : judge.play_hp();
                st.gauge_survival = course || !(judge.display_gauge_type() == kGroove || judge.display_gauge_type() == kEasy);
                set_gauge_tint(st, judge, app);
                st.scratch_angle = scratch_angle;
                st.bga = &bga_textures;
                st.bga_base = bga_base;
                st.bga_layer = bga_layer;
                st.bga_poor = bga_poor;
                st.bga_show_poor = cfg.bga != 3 && now_ms - last_miss_ms < cfg.poor_bga_ms;  // LR2: poorbga（既定 500ms）
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                app.skin_renderer->draw_play(*skin, st);
                if (!scripts.empty()) {
                    // MOD が描くもの（スキンの座標で、スキンの上に重ねる）
                    ModDrawState ds;
                    ds.now_ms = now_ms, ds.song_ms = ctx.song_ms;
                    ds.combo = judge.stats().combo, ds.exscore = judge.stats().exscore(), ds.hp = judge.play_hp();
                    ds.view_w = skin->width, ds.view_h = skin->height;
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    for (const ModDrawCmd& c : scripts.draw(ds)) {
                        const uint8_t cr = (c.rgb >> 16) & 0xff, cg = (c.rgb >> 8) & 0xff, cb = c.rgb & 0xff;
                        if (c.kind == ModDrawCmd::Kind::Rect) {
                            SDL_SetRenderDrawColor(renderer, cr, cg, cb, c.alpha);
                            SDL_FRect rc{c.x, c.y, c.w, c.h};
                            SDL_RenderFillRect(renderer, &rc);
                        } else {
                            overlay_text.draw(c.x, c.y, c.size, SDL_Color{cr, cg, cb, c.alpha}, c.text, c.align);
                        }
                    }
                    report_mod_errors();
                }
                if (int k = adjust_key()) {
                    // 調整している項目と値（LR2: Main.cpp:2176 の表示）
                    const SkinAdjust& a = st.adjust;
                    static const char* names[] = {"", "スキン位置の変更", "スキン拡大率の変更", "ジャッジ表示位置の変更", "ノートサイズの変更",
                                                  "スキン描画制限", "ノート位置の変更", "", "ジャッジ表示サイズの変更", "ノートだけの縦の位置の変更"};
                    static const char* darks[] = {"OFF", "DARK 1", "DARK 2"};
                    char v[64];
                    if (k == 5) std::snprintf(v, sizeof(v), "%s", darks[std::clamp(a.dark, 0, 2)]);
                    else if (k == 8) std::snprintf(v, sizeof(v), "%d%%", a.judge_scale);
                    else if (k == 9) std::snprintf(v, sizeof(v), "y:%d", a.note_fine_y);
                    else {
                        int x = k == 1 ? a.shift_x : k == 2 ? a.rate_x : k == 3 ? a.judge_x : k == 4 ? a.size_x : a.note_x;
                        int y = k == 1 ? a.shift_y : k == 2 ? a.rate_y : k == 3 ? a.judge_y : k == 4 ? a.size_y : a.note_y;
                        std::snprintf(v, sizeof(v), "x:%d  y:%d", x, y);
                    }
                    overlay_text.draw(4, 2, 14, SDL_Color{255, 255, 255, 255}, std::string(names[k]) + "（カーソルキーで調節）");
                    overlay_text.draw(4, 20, 14, SDL_Color{255, 255, 255, 255}, v);
                }
            } else if (!started) {
                draw_loading(renderer, chart, load_done.load(), total_sounds);
            } else {
                view.song_ms = song_ms;
                view.render_beat = render_beat;
                view.hi_speed = hs;
                view.speed_multiplier = scroll_multiplier;
                char status[128];
                std::snprintf(status, sizeof(status), "UP/DOWN: HI-SPEED  SHIFT+LEFT/RIGHT: TIMING %+d ms  F4: FULLSCREEN  ESC: END",
                              cfg.judge_timing);
                view.status = status;
                draw_play(renderer, chart, judge, view);
            }

            dev_save_frame(renderer, (SDL_GetTicksNS() - prelude_ns) / 1e6);
            if (!opt.capture_dir.empty() && now_ms / 1000 >= next_capture_s * 2) {
                if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "/frame_%03d%s.bmp", next_capture_s * 2,
                                  phase == Phase::Result ? "_result" : "");
                    SDL_SaveBMP(shot, (opt.capture_dir + name).c_str());
                    SDL_DestroySurface(shot);
                }
                ++next_capture_s;
                // 開発用：撮影中はリザルトを2秒ごとに自動で進める
                if (phase == Phase::Result) result_input();
            }
            {
                uint64_t d0 = SDL_GetTicksNS();
                frame_present(renderer);
                app.last_draw_ms = (SDL_GetTicksNS() - now_ns) / 1e6;
                app.last_present_ms = (SDL_GetTicksNS() - d0) / 1e6;
            }
            if (opt.quit_after > 0 && now_ms / 1000 > opt.quit_after) quit = true, done = true, exit = PlayExit::Quit;
        }
        app.scene_end_ns = SDL_GetTicksNS();
        mixer.stop();
        for (int id : {kSysDecide, kSysClear, kSysFail, kSysStop}) mixer.stop_system(id);
        mixer.wait_callback();
        mixer.fade_all(1.0f, 0);
        if (skin) app.set_view(skin->width, skin->height, false);
        if (course) {
            // 次のステージへ持ち越す。途中でやめたときは全ゲージが 0（LR2: Play.cpp:1659）
            const PlayStats& ps = judge.stats();
            course->hp = judge.gauge().hp_all();
            if (ctx.quit_early || fade_quit) course->hp.fill(0.0);
            course->gauge_type = judge.gauge_type();
            course->combo = ps.course_combo;
            course->max_combo = ps.course_max_combo;
            for (int k = 0; k < 6; ++k) course->total.judge_count[k] += ps.judge_count[k];
            course->total.empty_poor += ps.empty_poor;
            course->total.fast += ps.fast, course->total.slow += ps.slow;
            course->total.note_current += ps.note_current;
            course->total.total_notes += ps.total_notes;
            course->finished = ps.note_current >= ps.total_notes && !fade_quit;
            // コースリザルトのグラフのために、このステージの推移を残す（リザルトを出さずに進んだときは、ここで埋める）
            if (graph.cursor < PlayGraph::kPoints) {
                std::array<double, kGaugeCount> end_hp = course->hp;
                graph.finish(end_hp, ps.combo, ps.exscore(), 0, 0);
            }
            course->graphs.push_back({graph, static_cast<int>(std::max<int64_t>(1, chart.end_us / 1000))});
            course->alive = !quit && !course->abandoned && course->finished && course->hp[course->gauge_type] >= 2.0;
        }
    }
    if (!loader_joined) loader.join();
    for (SDL_Texture* t : bga_textures)
        if (t) SDL_DestroyTexture(t);
    for (SkinRenderer* sr : {app.skin_renderer.get(), app.result_renderer.get()})
        sr->set_image_override(100, nullptr), sr->set_image_override(101, nullptr);
    if (stage_tex) SDL_DestroyTexture(stage_tex);
    if (back_tex) SDL_DestroyTexture(back_tex);
    return exit;
}

namespace {

// コースリザルト（LR2: Scene13_Courseresult）。コース通しの値を、リザルトと同じ番号で見せる
PlayExit run_course_result(App& app, const CourseRun& cr, const PlayOptions& opt) {
    const Course& course = *cr.course;
    AppConfig& cfg = app.cfg;
    Mixer& mixer = app.mixer;
    const int n = static_cast<int>(course.stages.size());
    // コース通しの値。途中で終わったときは、遊ばなかったステージの総ノーツ数も足す（LR2: statplay.cpp:241）
    PlayStats total = cr.total;
    for (int i = cr.stage + 1; i < n; ++i)
        if (course.stages[i].chart) total.total_notes += course.stages[i].chart->notes;
    total.combo = cr.combo;
    total.max_combo = cr.max_combo;
    const int gtype = cr.gauge_type;
    const double hp = cr.hp[gtype];
    // コース通しのゲージの推移：ステージのグラフを、長さの割合でつなぐ（遊ばなかったステージの分は最後の値のまま）
    PlayGraph course_graph;
    if (!cr.graphs.empty()) {
        int64_t whole = 0;
        for (const auto& g : cr.graphs) whole += g.second;
        int64_t before = 0;
        int ex_before = 0;
        size_t s = 0;
        for (int i = 0; i < PlayGraph::kPoints; ++i) {
            const int64_t t = whole * i / PlayGraph::kPoints;
            while (s + 1 < cr.graphs.size() && t >= before + cr.graphs[s].second) {
                ex_before += cr.graphs[s].first.exscore[PlayGraph::kPoints - 1];
                before += cr.graphs[s].second, ++s;
            }
            const PlayGraph& g = cr.graphs[s].first;
            const int k = static_cast<int>(std::clamp<int64_t>((t - before) * PlayGraph::kPoints / cr.graphs[s].second, 0, PlayGraph::kPoints - 1));
            for (int gi = 0; gi < kGaugeCount; ++gi) course_graph.hp[gi][i] = g.hp[gi][k];
            course_graph.combo[i] = g.combo[k];
            course_graph.exscore[i] = ex_before + g.exscore[k];
        }
        course_graph.cursor = course_graph.best_cursor = course_graph.target_cursor = PlayGraph::kPoints;
    }
    // コースのランプ：最後のステージを生きて終えたときだけクリア。フルコンボはコース全体を 1 本のコンボでつないだとき
    int clear = 1;
    const bool reached = cr.stage == n - 1 && cr.finished;
    if (reached && hp >= 2.0) {
        if (total.max_combo == total.total_notes) clear = total.judge_count[kGood] == 0 ? 6 : 5;
        else if (hp > 2.0 && total.note_current == total.total_notes)
            clear = (gtype == kHard || gtype == kGAttack || gtype == kPAttack) ? 4 : gtype == kEasy ? 2 : 3;
    }
    // LR2 と同じく、コースのランプには EASY の上限をかけない（ステージは記録していない。LR2: Scene13_Courseresult.cpp）
    // 7k モードで遊んだコースは別の記録
    const int profile = app.mod_profile(cfg.seven_key);

    ScoreRecord best;
    const bool has_best = app.db.get_score(course.hash, best, 0, profile);
    PlayContext ctx;
    ctx.course = &course;
    ctx.course_stage = cr.stage;
    ctx.course_stages = n;
    ctx.stats_override = &total;
    ctx.hp_override = hp;
    ctx.gauge_type = gtype;
    ctx.autoplay = opt.autoplay;
    ctx.in_result = true;
    ctx.result_clear = clear;
    ctx.quit_early = !reached;
    ctx.old_exscore = has_best ? best.exscore : 0;
    ctx.old_maxcombo = has_best ? best.max_combo : 0;
    ctx.old_minbp = has_best ? best.bp : -1;
    ctx.old_total_notes = has_best ? total.total_notes : 0;
    ctx.best_clear = has_best ? best.clear : 0;
    ctx.best_exscore = ctx.old_exscore;
    if (opt.save_score && !opt.autoplay) {
        PlayRecord rec;
        rec.profile = profile;
        rec.clear = clear, rec.exscore = total.exscore(), rec.max_combo = total.max_combo;
        rec.judge = {total.judge_count[kPGreat], total.judge_count[kGreat], total.judge_count[kGood], total.judge_count[kBad],
                     total.judge_count[kPoor]};
        // 途中で終わったときは、残りのノーツも BP に入れる
        rec.bp = total.bp() + total.total_notes - total.note_current;
        rec.op_history = option_history(clear, gtype, cfg);
        app.db.save_score(course.hash, rec);
        ctx.best_clear = std::max(ctx.best_clear, clear);
    }
    std::printf("コースの結果: %s ランプ %d / EX %d / MAX COMBO %d / %d ノーツ\n", course.name.c_str(), clear, total.exscore(),
                total.max_combo, total.total_notes);
    mixer.play_system(clear < 2 ? kSysCourseFail : kSysCourseClear, true);  // リザルトの間は繰り返す（LR2 と同じ）
    ctx.result_flip = app.play_skin && app.play_skin->flip_result;
    app.load_courseresult_skin(ctx.load_options());
    Lr2Skin* rskin = app.courseresult_skin.get();
    if (!rskin) return PlayExit::Back;
    ctx.result_flip = rskin->flip_result;
    ctx.skin = rskin;
    app.set_view(rskin->width, rskin->height, false);

    InputMapper input(app.keycfg);
    input.set_min_hold_ms(cfg.input_interval);
    SkinTimers rtimers;
    rtimers.set(0, 0);
    const uint64_t start_ns = SDL_GetTicksNS();
    bool audio_fading = false;
    int chart_op3 = rskin->gaugechart[0].src.op[2], chart_op4 = rskin->gaugechart[0].src.op[3];
    // 決定で グラフ → ランク → 更新 → フェードアウト と進む（リザルトと同じ）
    auto result_input = [&] {
        double t = (SDL_GetTicksNS() - start_ns) / 1e6;
        if (t <= rskin->startinput) return;
        if (!rtimers.on(151)) {
            rtimers.set(151, t);
            chart_op4 = static_cast<int>(t);
        } else if (rtimers.lapse(151, t) <= rskin->startinput_rank || rtimers.on(152)) {
            if (rtimers.lapse(152, t) > rskin->startinput_update && !rtimers.on(2)) rtimers.set(2, t);
        } else if (!opt.save_score || opt.autoplay) {
            if (!rtimers.on(2)) rtimers.set(2, t);
        } else {
            rtimers.set(152, t);
        }
    };
    PlayExit exit = PlayExit::Back;
    for (bool done = false; !done;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) done = true, exit = PlayExit::Quit;
            else if (e.type == SDL_EVENT_JOYSTICK_ADDED) SDL_OpenJoystick(e.jdevice.which);
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                     (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_RETURN))
                result_input();
            input.handle(e);
        }
        joystick_init().open_new();
        std::vector<LaneInput> li;
        std::vector<ButtonInput> bi;
        input.collect(SDL_GetTicksNS(), li, bi);
        for (const ButtonInput& b : bi)
            if (b.down && b.button <= kKey7) result_input();
        double t = (SDL_GetTicksNS() - start_ns) / 1e6;
        if (t > chart_op3 && !rtimers.on(150)) rtimers.set(150, t);
        if (t > chart_op4 && !rtimers.on(151)) rtimers.set(151, t);
        if (rtimers.on(2) && !audio_fading) mixer.fade_all(0.0f, rskin->fadeout), audio_fading = true;
        if (rtimers.on(2) && rtimers.lapse(2, t) >= rskin->fadeout) done = true;
        ctx.now_ms = t;
        PlaySkinState st;
        st.now_ms = t;
        st.timers = &rtimers;
        st.option = [&](int o) { return ctx.option(o); };
        st.number = [&](int id) { return ctx.number(id); };
        st.bargraph = [&](int ty, double& v, double& m) { return ctx.bargraph(ty, v, m); };
        st.slider = [&](int ty, int& v, int& lo, int& hi) { return ctx.slider(ty, v, lo, hi); };
        st.text = [&](int id) { return ctx.text(id); };
        st.button = [&](int ty) { return ctx.button(ty); };
        st.graph = cr.graphs.empty() ? nullptr : &course_graph;
        st.graph_gauge = gtype;
        st.gauge_survival = true;  // コースのゲージは、どれも落ちたら終わり
        st.total_notes = total.total_notes;
        st.chart_op3 = chart_op3;
        st.chart_op4 = chart_op4;
        st.result_flip = ctx.result_flip;
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        app.result_renderer->draw_result(*rskin, st);
        if (const char* shot = dev_env("BMJ_COURSE_SHOT"); shot && t > 3000 && !rtimers.on(2)) {
            // 開発用：3 秒たったら 1 枚保存して抜ける
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            rtimers.set(2, t);
        }
        frame_present(app.renderer);
    }
    for (int id : {kSysCourseClear, kSysCourseFail}) mixer.stop_system(id);
    app.scene_end_ns = SDL_GetTicksNS();  // 選曲画面に戻るまでの時間は、コースリザルトを閉じたところから数える
    mixer.wait_callback();
    mixer.fade_all(1.0f, 0);
    return exit;
}

}  // namespace

PlayExit run_course(App& app, const Course& course, PlayOptions opt) {
    AppConfig& cfg = app.cfg;
    if (app.metronome()) opt.save_score = false;  // メトロノームを鳴らして遊んだコースは記録しない
    // 段位認定はオプションを書き換える（LR2: Main.cpp:1546）。配置は正規だけ（難易度表の指定で MIRROR などを許す）、
    // EASY と G-ATTACK は GROOVE に、
    // HIDDEN/SUDDEN は切る。LR2 は終わっても戻さないが、BMjoy はコースが終わったら元に戻す。
    // ハイスピード固定（ノースピ）のコースは、ハイスピ 1.00・HS-FIX なし・カバーと LIFT なし・基本スクロールスピード 100% で遊ぶ
    const AppConfig saved = cfg;
    struct Restore {
        std::function<void()> f;
        ~Restore() { f(); }
    } restore{[&] {
        if (course.type == 2) cfg.random = saved.random, cfg.gauge = saved.gauge, cfg.effect = saved.effect;
        if (course.no_speed) {
            cfg.hs_mode = saved.hs_mode, cfg.hi_speed = saved.hi_speed, cfg.hs_fix = saved.hs_fix, cfg.lane_cover = saved.lane_cover;
            cfg.lift = saved.lift, cfg.base_speed = saved.base_speed;
        }
        if (course.ln_mode >= 0) cfg.ln_mode = saved.ln_mode;
        if (course.type == 2 || course.no_speed || course.ln_mode >= 0) app.save_config();
    }};
    // LN の種類の制約は、コースの間だけプレイヤーの設定を書き換える
    if (course.ln_mode >= 0) cfg.ln_mode = course.ln_mode;
    if (course.no_speed) {
        cfg.hs_mode = 0, cfg.hi_speed = 100, cfg.hs_fix = 0, cfg.lane_cover = 0;
        cfg.lift = false, cfg.base_speed = 100;
    }
    if (course.type == 2) {
        // 使えない配置は正規に戻す（MIRROR 可なら MIRROR まで、RANDOM 可なら RANDOM まで）
        int allowed = course.grade_random == 0 ? kRandomOff : course.grade_random == 1 ? kMirror : kRandom;
        if (cfg.random > allowed) cfg.random = kRandomOff;
        if (cfg.gauge == kEasy || cfg.gauge == kGAttack) cfg.gauge = kGroove;
        cfg.effect = 0;
    }
    CourseRun cr;
    cr.course = &course;
    cr.gauge_type = cfg.gauge;
    const int n = static_cast<int>(course.stages.size());
    for (int i = 0; i < n; ++i) {
        cr.stage = i;
        PlayOptions o = opt;
        o.course = &cr;
        o.decide_sound = i == 0 && opt.decide_sound;  // 2 ステージ目からは決定画面も決定の音も無い
        if (run_play(app, course.stages[i].chart->path, o) == PlayExit::Quit) return PlayExit::Quit;
        app.save_config();
        if (cr.abandoned) return PlayExit::Back;
        if (!cr.alive) break;
    }
    return run_course_result(app, cr, opt);
}

}  // namespace bmj
