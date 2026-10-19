#include "app/play_session.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
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
#include "input/input.h"
#include "input/joystick_init.h"
#include "render/frame_hooks.h"
#include "render/play_scene.h"
#include "render/skin_play.h"
#include "render/text_cache.h"
#include "util/hash.h"
#include "util/path.h"
#include "video/video_decoder.h"

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
// 皿の自動演奏でのクリアは 1 ビットだけ。EASY ランプは EASY ゲージのときだけ。それより上のランプはゲージ・ランダム・HIDDEN/SUDDEN を残す
uint32_t option_history(int lamp, int gauge_type, const AppConfig& cfg, bool assist) {
    if (assist) return lamp > 1 ? 0x1000000u : 0u;
    if (lamp == 2) return gauge_type == kEasy ? 8u : 0u;
    if (lamp > 2) return (1u << gauge_type) | (0x100u << std::clamp(cfg.random, 0, 7)) | (0x10000u << std::clamp(cfg.effect, 0, 3));
    return 0;
}

// lamp_only：ランプ・回数・オプション履歴だけを残す（皿の自動演奏・CONSTANT・SCATTER・CONVERGE。LR2: statplay.cpp:895）
// ghost は判定の列、rseed は譜面の乱数の種（自己ベストを更新したときだけ残る）
void record_score(App& app, const std::string& sha256, const JudgeEngine& j, int lamp, const AppConfig& cfg, bool assist,
                  bool lamp_only, bool count_only, bool course_stage, int ln_mode, const std::vector<int8_t>& ghost, uint32_t rseed) {
    const PlayStats& s = j.stats();
    PlayRecord rec;
    rec.count_only = count_only;
    rec.ghost = encode_ghost(ghost);
    rec.rseed = static_cast<int>(rseed);
    rec.op_best = j.gauge_type() + std::clamp(cfg.random, 0, 9) * 10;  // LR2: ゲージ + 配置×10
    rec.course_stage = course_stage;
    rec.lamp_only = lamp_only;
    rec.ln_mode = ln_mode;
    rec.clear = lamp, rec.exscore = s.exscore(), rec.max_combo = s.max_combo;
    rec.judge = {s.judge_count[kPGreat], s.judge_count[kGreat], s.judge_count[kGood], s.judge_count[kBad], s.judge_count[kPoor]};
    rec.op_history = option_history(lamp, j.gauge_type(), cfg, assist);
    // 処理しなかったノーツは、どう終わっても BP に入れる（途中でやめたときも閉店したときも。LR2: statplay.cpp:992）
    rec.bp = s.bp() + s.total_notes - s.note_current;
    app.db.save_score(sha256, rec);
    // 遅くして遊んだときは、ふだんの記録には回数しか残さない代わりに、中身を別の表に残す（画面には出さない）
    if (count_only) app.db.log_play(sha256, rec, "slow", cfg.sound_fx.play_rate());
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
    std::printf("  ゲージ %.1f%%\n", j.gauge().hp(j.gauge_type()));
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

PlayExit run_play(App& app, const std::string& chart_path, PlayOptions opt) {
    // スコアは譜面ファイルの原本のSHA-256で記録する（仕様書8章）
    std::string sha256;
    {
        std::ifstream f(chart_path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sha256 = sha256_hex(bytes);
    }
    AppConfig& cfg = app.cfg;
    // リプレイの再生：記録したときの乱数の種とオプションで遊ぶ。オプションは終わったら戻す（LR2: SetReplayConfig）
    Replay replay;
    const AppConfig saved_cfg = cfg;
    struct Restore {
        std::function<void()> f;
        ~Restore() { f(); }
    } restore_replay{[&] {
        if (!opt.replay) return;
        cfg.random = saved_cfg.random, cfg.gauge = saved_cfg.gauge, cfg.ln_mode = saved_cfg.ln_mode, cfg.hs_fix = saved_cfg.hs_fix;
        cfg.effect = saved_cfg.effect, cfg.assist = saved_cfg.assist, cfg.gas = saved_cfg.gas, cfg.hi_speed = saved_cfg.hi_speed;
        cfg.lane_cover = saved_cfg.lane_cover, cfg.lane_cover_v = saved_cfg.lane_cover_v, cfg.judge_timing = saved_cfg.judge_timing;
    }};
    // 開発用：BMJ_INPUT_REPLAY のファイルの入力を、手で押したものとして流す（記録・ゴースト・リプレイの保存はふだんどおり）
    bool feed_input = false;
    if (const char* f = std::getenv("BMJ_INPUT_REPLAY"); f && !opt.replay && !opt.autoplay) feed_input = load_replay(f, replay);
    if (opt.replay) {
        if (!load_replay(replay_path(sha256), replay) || replay.sha256 != sha256) {
            std::fprintf(stderr, "リプレイが無い: %s\n", chart_path.c_str());
            opt.replay = false;
            return PlayExit::Back;
        }
        cfg.random = replay.random, cfg.gauge = replay.gauge, cfg.ln_mode = replay.ln_mode, cfg.hs_fix = replay.hs_fix;
        cfg.effect = replay.effect, cfg.assist = replay.assist, cfg.gas = replay.gas, cfg.hi_speed = replay.hi_speed;
        cfg.lane_cover = replay.lane_cover, cfg.lane_cover_v = replay.lane_cover_v;
        opt.seed = replay.seed, opt.save_score = false, opt.autoplay = false;
    }
    BmsParseOptions popt;
    popt.random_seed = opt.seed ? opt.seed : static_cast<uint32_t>(SDL_GetTicksNS() % 0x7fff);
    BmsParseResult parsed = parse_chart_file(chart_path, popt);
    if (!parsed.ok) {
        std::fprintf(stderr, "読み込めない: %s\n", parsed.error.c_str());
        return PlayExit::Back;
    }
    // 配置のオプション（LR2: bmsload:3354）。#RANDOM を評価した乱数の続きで並べ替える
    apply_random_option(parsed.chart, cfg.random, parsed.rng);
    // TOTAL の既定式は、並べ替えで重なったノーツを捨てたあとの数で（LR2: bmsload:3789）
    if (!parsed.chart.total_defined) parsed.chart.total = lr2_default_total(parsed.chart.judgeable_notes());
    // CONVERGE のときは皿の自動演奏を切る（LR2: bmsload:2102）
    const bool assist = cfg.assist && cfg.random != kConverge;
    // HS-FIX の CONSTANT：一定の速さで流す
    const bool constant = cfg.hs_fix == 4;
    if (constant) parsed.chart.make_constant_speed();
    // 皿の自動演奏・CONSTANT・SCATTER・CONVERGE は、ランプを EASY まで（フルコンボも EASY）にして、
    // ランプ・回数・オプション履歴だけを残す。コースのステージでは何も残さない（LR2: statplay.cpp:7, 895）
    const bool lamp_only = assist || constant || cfg.random >= kScatter;
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
    const double fhs_bpm = constant ? 150.0 : floating_base_bpm(chart, cfg.fhs_base);
    int fhs = 100;  // フローティングのハイスピ（100 = 1.00）
    int& hs = floating ? fhs : cfg.hi_speed;
    int start_green = cfg.fhs_green;  // 曲の始まりの緑数字（ギアチェンの「緑数字に戻す」で使う）
    // 記録は LN の種類ごとに分ける（種類を決めていない LN が無い譜面は 0 にまとめる）
    const int score_ln_mode = chart.free_ln_count() > 0 ? cfg.ln_mode : 0;
    ScoreRecord best_score;
    bool has_best = app.db.get_score(sha256, best_score, score_ln_mode);
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
    ctx.assist = assist;
    ctx.bga_shown = bga_shown;
    ctx.hs_fix = cfg.hs_fix;
    {
        // 同じフォルダの、難易度ごとの譜面のレベル（数値45〜49）
        // 曲データベースのパスは絶対パスなので、合わせてから比べる
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(chart_path, ec).lexically_normal();
        std::string folder = abs.parent_path().string();
        for (const ChartRecord& c : app.db.all_charts()) {
            if (c.folder != folder || c.difficulty < 1 || c.difficulty > 5) continue;
            int& lv = ctx.diff_level[c.difficulty - 1];
            if (lv < 0 || c.path == abs.string()) lv = c.playlevel;
        }
        // 曲データベースに無い譜面でも、自分の難易度のところには自分のレベルを入れる
        int d = chart.meta.difficulty;
        if (d >= 1 && d <= 5) ctx.diff_level[d - 1] = chart.meta.playlevel;
    }
    ctx.gauge_type = cfg.gauge;
    app.load_play_skin(ctx.load_options());
    Lr2Skin* skin = app.play_skin.get();
    ctx.skin = skin;
    // 判定ラインまでのレーンの長さ（スキンの座標）。緑数字と LIFT の計算に使う
    const double lane_px = skin && !skin->note_dst[1].empty() ? skin->note_dst[1].keys.front().y : 480.0;
    if (skin) app.set_view(skin->width, skin->height);
    else app.set_view(640, 480);

    // キー音はワーカーで読み込む。読み込み中もスキンの読み込み画面を動かす
    std::vector<std::string> missing;
    std::vector<std::string> paths = resolve_chart_sounds(chart, missing);
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
        bank.load(paths, mixer.sample_rate(), threads, &load_errors, &load_done, &ranges);
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
        jcfg.assist_scratch = assist;
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
        }
        JudgeEngine judge(chart, jcfg);
        ctx.judge = &judge;
        ctx.timers = SkinTimers();
        ctx.song_ms = -1;
        ctx.judge_draw = 0;
        ctx.last_miss_ms = -1e9;
        // 自己ベストとターゲットのゴースト（LR2: Scene04_Play.cpp:1969）。リトライでは、記録し直した自己ベストを読み直す
        if (retried) has_best = app.db.get_score(sha256, best_score, score_ln_mode);
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
        const uint64_t scene_start = SDL_GetTicksNS();
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
        // リザルト画面（LR2: Scene05_Result.cpp）。タイマーはプレイ画面とは別
        SkinTimers rtimers;
        uint64_t result_start_ns = 0;
        int chart_op3 = 0, chart_op4 = 0;
        Lr2Skin* rskin = nullptr;
        auto enter_result = [&](bool quit_early) {
            mixer.stop();
            print_result(chart, judge);
            const PlayStats& ps = judge.stats();
            // LR2（Main.cpp:1697）：オートプレイ・1 ノーツも判定していない・GOOD 以上が 1 つも無いときは、
            // リザルトを出さず、記録もせずに選曲画面へ戻る
            ctx.quit_early = quit_early;
            if (opt.autoplay || ps.note_current == 0 ||
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
            ctx.quit_early = quit_early;
            {
                // 演奏中に押されたままの入力をリザルトに持ち込まない
                std::vector<LaneInput> li;
                std::vector<ButtonInput> bi;
                input.collect(SDL_GetTicksNS() + 100000000ull, li, bi);
            }
            ctx.result_clear = (quit_early || judge.failed()) ? 1 : judge.clear();
            if (lamp_only && ctx.result_clear > 2) ctx.result_clear = 2;
            // 比較に使う「このプレイの前の自己ベスト」は、記録する前に取っておく
            ctx.old_exscore = has_best ? best_score.exscore : 0;
            ctx.old_maxcombo = has_best ? best_score.max_combo : 0;
            ctx.old_minbp = has_best ? best_score.bp : -1;
            ctx.old_total_notes = has_best ? ps.total_notes : 0;
            if (!recorded && opt.save_score && !opt.autoplay && started && !(course && (lamp_only || slowed))) {
                record_score(app, sha256, judge, ctx.result_clear, cfg, assist, lamp_only, slowed, course != nullptr, score_ln_mode,
                             ghost_log, popt.random_seed);
                // LR2はリザルトの前に記録するので、リザルトで見える自己ベストのランプ（100-105）は更新後のもの
                // （コースのステージではフルコンボのときだけ上がる）
                if ((!course || ctx.result_clear >= 5) && !slowed) ctx.best_clear = std::max(ctx.best_clear, ctx.result_clear);
            }
            // リプレイの保存（LR2: replaysave。Main.cpp:1796）。ALWAYS は毎回、HISCORE は EX が自己ベスト以上、
            // CLEAR はクリア、FULLCOMBO はフルコンボのとき（どれも EX が 0 なら残さない）。コースのステージは残さない
            if (!recorded && opt.save_score && !opt.autoplay && started && !course && cfg.replay_save > 0) {
                const int ex = ps.exscore();
                bool keep = cfg.replay_save == 1 || (ex > 0 && ((cfg.replay_save == 2 && ex >= ctx.old_exscore) ||
                                                              (cfg.replay_save == 3 && ctx.result_clear >= 2) ||
                                                              (cfg.replay_save == 4 && ctx.result_clear >= 5)));
                if (keep) {
                    record.sha256 = sha256;
                    record.seed = popt.random_seed;
                    record.random = cfg.random, record.gauge = cfg.gauge, record.ln_mode = cfg.ln_mode, record.hs_fix = cfg.hs_fix;
                    record.effect = cfg.effect, record.assist = cfg.assist, record.gas = cfg.gas, record.hi_speed = cfg.hi_speed;
                    record.lane_cover = cfg.lane_cover, record.lane_cover_v = cfg.lane_cover_v;
                    if (!save_replay(replay_path(sha256), record)) std::fprintf(stderr, "リプレイを保存できない\n");
                }
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
        // スキンの位置の調整で押している数字キー（1〜6。押していなければ 0）
        auto adjust_key = [] {
            const bool* ks = SDL_GetKeyboardState(nullptr);
            const SDL_Scancode keys[] = {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6};
            for (int i = 0; i < 6; ++i)
                if (ks[keys[i]]) return i + 1;
            return 0;
        };
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
                if (b.button == kScratchA || b.button == kScratchB) {
                    // START か SELECT を押しながら皿（時計回りで + ）。
                    // フローティング：カバーがあればカバーを動かし、緑数字が変わらないようにハイスピを決め直す（ギアチェン）。
                    // LIFT を使っていてカバーが無ければ LIFT。どちらも無ければハイスピの微調整（0.01）。
                    // クラシック：LIFT を使っていてカバーが無ければ LIFT（OpenLR2 と同じ）
                    int d = b.button == kScratchB ? 1 : -1;
                    if (floating && cover_active && cover_movable()) {
                        cfg.lane_cover_v = std::clamp(cfg.lane_cover_v + d, 0, 100);
                        keep_green(cfg.fhs_green);
                    } else if (cfg.lift && !cover_active && cover_movable()) {
                        cfg.lift_v = std::clamp(cfg.lift_v + d, 0, 100);
                        if (floating) keep_green(cfg.fhs_green);
                    } else if (floating) {
                        hs = std::clamp(hs + d, kFloatingHsMin, kFloatingHsMax);
                        hs_changed();
                    }
                    continue;
                }
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
                case SDL_EVENT_MOUSE_WHEEL:
                    // ホイールでレーンカバー（LR2: gameloop:183）
                    if (phase == Phase::Play && cfg.lane_cover == 1 && cover_movable() && !speed_locked) {
                        cfg.lane_cover_v = std::clamp(cfg.lane_cover_v - static_cast<int>(std::lround(e.wheel.y)), 0, 100);
                        if (floating) keep_green(cfg.fhs_green);
                    }
                    break;
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
                        SkinAdjust& a = cfg.skin_adjust[skin->path];
                        int dx = e.key.key == SDLK_RIGHT ? 1 : e.key.key == SDLK_LEFT ? -1 : 0;
                        int dy = e.key.key == SDLK_DOWN ? 1 : e.key.key == SDLK_UP ? -1 : 0;
                        switch (adjust_key()) {
                        case 1: a.shift_x += dx * 10, a.shift_y += dy * 10; break;
                        case 2: a.rate_x += dx * 5, a.rate_y += dy * 5; break;
                        case 3: a.judge_x += dx * 10, a.judge_y += dy * 10; break;
                        case 4: a.size_x += dx, a.size_y -= dy; break;
                        case 5: a.dark = ((a.dark + (dy != 0 ? -dy : -dx)) % 3 + 3) % 3; break;
                        case 6: a.note_x += dx * 10, a.note_y += dy * 10; break;
                        default: break;
                        }
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
                double hp = judge.gauge().hp(judge.gauge_type());
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
            if (!ctx.timers.on(1) && now_ms >= startinput) ctx.timers.set(1, now_ms);
            if (!ctx.timers.on(40) && loader_joined && now_ms >= load_wait) ctx.timers.set(40, now_ms);
            // 曲は #PLAYSTART が過ぎ、START・SELECT と数字キー（1〜8・0）が離されてから始める（LR2: Play.cpp:1755）
            auto start_blocked = [&] {
                if (input.button_held(kStart) || input.button_held(kSelect)) return true;
                const bool* ks = SDL_GetKeyboardState(nullptr);
                for (SDL_Scancode sc : {SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
                                        SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_0})
                    if (ks[sc]) return true;
                return false;
            };
            if (ctx.timers.on(40) && !started && ctx.timers.lapse(40, now_ms) >= playstart && !start_blocked()) {
                std::vector<ScheduledSound> schedule;
                uint32_t rate = mixer.sample_rate();
                auto frame_of = [&](int64_t us) { return us * static_cast<int64_t>(rate) / 1000000; };
                for (const BgmEvent& b : chart.bgm) schedule.push_back({frame_of(b.time_us), b.sound});
                if (opt.autoplay)
                    for (const Note& n : chart.notes)
                        if ((n.kind == NoteKind::Normal || n.kind == NoteKind::Long) && n.sound)
                            schedule.push_back({frame_of(n.time_us), n.sound, true});
                mixer.set_song(&bank.samples(), std::move(schedule));
                mixer.stop_system(kSysDecide);
                mixer.start(0);
                started = true;
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
                        if (je.has_offset) ctx.last_offset_ms = je.offset_ms;
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
                if (!last_note_set && ps.note_current >= ps.total_notes) ctx.timers.set(143, now_ms), last_note_set = true;
                if (!fc_set && ps.total_notes > 0 && ps.combo == ps.total_notes) ctx.timers.set(48, now_ms), fc_set = true;
                // ゲージのタイマー（LR2: ApplyJudgeNote の 42/44）
                int hp = static_cast<int>(judge.gauge().hp(judge.gauge_type())) / 2 * 2;
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
                bool survival = course || !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                if (survival && judge.failed()) {
                    phase = Phase::Failed;
                    ctx.timers.set(3, now_ms);
                    mixer.stop();
                    mixer.play_system(kSysStop);  // 閉店（LR2: Play.cpp:1367）
                    view.failed = true;
                } else if (song_us > chart.end_us) {
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

            if (std::getenv("BMJ_DEBUG_LOOP")) {
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
                st.graph_gauge = judge.gauge_type();
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
                st.speed_multiplier = speed_multiplier;
                st.lift_px = cfg.lift ? static_cast<float>(lane_px * cfg.lift_v / 100.0) : 0.0f;
                if (auto it = cfg.skin_adjust.find(skin->path); it != cfg.skin_adjust.end()) st.adjust = it->second;
                st.base_speed = cfg.base_speed;
                st.effect = cfg.effect;
                st.autoplay = opt.autoplay;
                st.judge_draw = ctx.judge_draw;
                st.show_judge = !silent;
                // 判定の絵の横のコンボは、コースならコース通し（LR2: Play.cpp:237）
                st.combo_draw = course ? judge.stats().course_combo : judge.stats().combo;
                st.gauge_hp = ctx.hp_print >= 0 ? ctx.hp_print : judge.gauge().hp(judge.gauge_type());
                st.gauge_survival = course || !(judge.gauge_type() == kGroove || judge.gauge_type() == kEasy);
                st.scratch_angle = scratch_angle;
                st.bga = &bga_textures;
                st.bga_base = bga_base;
                st.bga_layer = bga_layer;
                st.bga_poor = bga_poor;
                st.bga_show_poor = cfg.bga != 3 && now_ms - last_miss_ms < cfg.poor_bga_ms;  // LR2: poorbga（既定 500ms）
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                app.skin_renderer->draw_play(*skin, st);
                if (int k = adjust_key()) {
                    // 調整している項目と値（LR2: Main.cpp:2176 の表示）
                    const SkinAdjust& a = st.adjust;
                    static const char* names[] = {"", "スキン位置の変更", "スキン拡大率の変更", "ジャッジ表示位置の変更", "ノートサイズの変更",
                                                  "スキン描画制限", "ノート位置の変更"};
                    static const char* darks[] = {"OFF", "DARK 1", "DARK 2"};
                    char v[64];
                    if (k == 5) std::snprintf(v, sizeof(v), "%s", darks[std::clamp(a.dark, 0, 2)]);
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
                view.speed_multiplier = speed_multiplier;
                char status[128];
                std::snprintf(status, sizeof(status), "UP/DOWN: HI-SPEED  SHIFT+LEFT/RIGHT: TIMING %+d ms  F4: FULLSCREEN  ESC: END",
                              cfg.judge_timing);
                view.status = status;
                draw_play(renderer, chart, judge, view);
            }

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
                frame_hooks_before_present(renderer);
                SDL_RenderPresent(renderer);
                app.last_draw_ms = (SDL_GetTicksNS() - now_ns) / 1e6;
                app.last_present_ms = (SDL_GetTicksNS() - d0) / 1e6;
            }
            if (opt.quit_after > 0 && now_ms / 1000 > opt.quit_after) quit = true, done = true, exit = PlayExit::Quit;
        }
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
    // コースのランプ：最後のステージを生きて終えたときだけクリア。フルコンボはコース全体を 1 本のコンボでつないだとき
    int clear = 1;
    const bool reached = cr.stage == n - 1 && cr.finished;
    if (reached && hp >= 2.0) {
        if (total.max_combo == total.total_notes) clear = total.judge_count[kGood] == 0 ? 6 : 5;
        else if (hp > 2.0 && total.note_current == total.total_notes)
            clear = (gtype == kHard || gtype == kGAttack || gtype == kPAttack) ? 4 : gtype == kEasy ? 2 : 3;
    }
    // LR2 と同じく、コースのランプには EASY の上限をかけない（ステージは記録していない。LR2: Scene13_Courseresult.cpp）
    const bool assist = cfg.assist && cfg.random != kConverge;

    ScoreRecord best;
    const bool has_best = app.db.get_score(course.hash, best);
    PlayContext ctx;
    ctx.course = &course;
    ctx.course_stage = cr.stage;
    ctx.course_stages = n;
    ctx.stats_override = &total;
    ctx.hp_override = hp;
    ctx.gauge_type = gtype;
    ctx.autoplay = opt.autoplay;
    ctx.assist = assist;
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
        rec.clear = clear, rec.exscore = total.exscore(), rec.max_combo = total.max_combo;
        rec.judge = {total.judge_count[kPGreat], total.judge_count[kGreat], total.judge_count[kGood], total.judge_count[kBad],
                     total.judge_count[kPoor]};
        // 途中で終わったときは、残りのノーツも BP に入れる
        rec.bp = total.bp() + total.total_notes - total.note_current;
        rec.op_history = option_history(clear, gtype, cfg, assist);
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
        st.graph_gauge = gtype;
        st.total_notes = total.total_notes;
        st.chart_op3 = chart_op3;
        st.chart_op4 = chart_op4;
        st.result_flip = ctx.result_flip;
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        app.result_renderer->draw_result(*rskin, st);
        if (const char* shot = std::getenv("BMJ_COURSE_SHOT"); shot && t > 3000 && !rtimers.on(2)) {
            // 開発用：3 秒たったら 1 枚保存して抜ける
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            rtimers.set(2, t);
        }
        frame_hooks_before_present(app.renderer);
        SDL_RenderPresent(app.renderer);
    }
    for (int id : {kSysCourseClear, kSysCourseFail}) mixer.stop_system(id);
    mixer.wait_callback();
    mixer.fade_all(1.0f, 0);
    return exit;
}

}  // namespace

PlayExit run_course(App& app, const Course& course, PlayOptions opt) {
    AppConfig& cfg = app.cfg;
    // 段位認定はオプションを書き換える（LR2: Main.cpp:1546）。配置は正規だけ（難易度表の指定で MIRROR などを許す）、
    // EASY と G-ATTACK は GROOVE に、
    // HIDDEN/SUDDEN と皿の自動演奏は切る。LR2 は終わっても戻さないが、BMjoy はコースが終わったら元に戻す。
    // ハイスピード固定（ノースピ）のコースは、ハイスピ 1.00・HS-FIX なし・カバーと LIFT なし・基本スクロールスピード 100% で遊ぶ
    const AppConfig saved = cfg;
    struct Restore {
        std::function<void()> f;
        ~Restore() { f(); }
    } restore{[&] {
        if (course.type == 2) cfg.random = saved.random, cfg.gauge = saved.gauge, cfg.effect = saved.effect, cfg.assist = saved.assist;
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
        cfg.assist = false;
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
