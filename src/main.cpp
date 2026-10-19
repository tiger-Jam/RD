// BMjoy：譜面を1つ指定して、LR2スキン（または仮の画面）で遊ぶ
//
//   bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]
//                        [--hs 100] [--seed N] [--timing ms] [--lr2-root DIR] [--skin FILE]
//                        [--skin-opt 番号=選択] [--skin-res sd|hd|fhd] [--no-skin] [--fps N] [--vsync]
//                        [--simulate] [--info]
//   bmjoy                    選曲画面
//   bmjoy --keyconfig
//   bmjoy --add-song-dir DIR --add-table URL   曲フォルダ・難易度表を登録する
//   bmjoy --import-lr2 LR2files/Database/Score/名前.db   LR2のスコアを取り込む
//
// 指定したオプションは設定ファイル（config.txt）に保存され、次からは指定しなくても使われる
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <future>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/mixer.h"
#include "app/play_session.h"
#include "app/select_scene.h"
#include "chart/bms_parser.h"
#include "chart/chart_loader.h"
#include "db/difficulty_table.h"
#include "db/song_db.h"
#include "game/judge.h"
#include "input/joystick_init.h"
#include "input/win_rawhid.h"
#include "render/keyconfig_scene.h"
#include "render/text_cache.h"
#include "skin/lr2_skin.h"

using namespace bmj;

JoystickInit& bmj::joystick_init() {
    static JoystickInit j;
    return j;
}

RawHid& bmj::raw_hid() {
    static RawHid r;
    return r;
}

namespace {

struct Options {
    std::string chart_path;
    PlayOptions play;
    bool simulate = false, info = false, keyconfig = false, no_skin = false, vsync = false;
    std::string lr2_root, skin, skin_res, result_skin;
    std::vector<std::string> scan_dirs;  // 開発用：曲フォルダを読んで結果を表示する
    std::string db_path;
    std::vector<std::string> table_urls;  // 開発用：難易度表を取得して表示する
    std::vector<std::string> add_song_dirs, add_tables;
    std::string import_lr2;  // LR2のスコアデータベース
    std::vector<std::pair<int, int>> skin_opts;
    int gauge = -1, hi_speed = -1, timing = 1000, fps = -1000, effect = -1;
    bool gas = false;
    bool latency_test = false, exclusive = false;  // 開発用：音の遅れを測る
    int buffer = 0;
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--auto") o.play.autoplay = true;
        else if (a == "--gas") o.gas = true;
        else if (a == "--simulate") o.simulate = true;
        else if (a == "--info") o.info = true;
        else if (a == "--no-skin") o.no_skin = true;
        else if (a == "--keyconfig") o.keyconfig = true;
        else if (a == "--latency-test") o.latency_test = true;
        else if (a == "--exclusive") o.exclusive = true;
        else if (a == "--buffer") o.buffer = std::atoi(next());
        else if (a == "--vsync") o.vsync = true;
        else if (a == "--capture") o.play.capture_dir = next();
        else if (a == "--quit-after") o.play.quit_after = std::atof(next());
        else if (a == "--lr2-root") o.lr2_root = next();
        else if (a == "--skin") o.skin = next();
        else if (a == "--result-skin") o.result_skin = next();
        else if (a == "--skin-res") o.skin_res = next();
        else if (a == "--fps") o.fps = std::atoi(next());
        else if (a == "--scan") o.scan_dirs.push_back(next());
        else if (a == "--db") o.db_path = next();
        else if (a == "--table") o.table_urls.push_back(next());
        else if (a == "--add-song-dir") o.add_song_dirs.push_back(next());
        else if (a == "--add-table") o.add_tables.push_back(next());
        else if (a == "--import-lr2") o.import_lr2 = next();
        else if (a == "--skin-opt") {
            std::string v = next();
            size_t eq = v.find('=');
            if (eq == std::string::npos) return false;
            o.skin_opts.push_back({std::atoi(v.substr(0, eq).c_str()), std::atoi(v.substr(eq + 1).c_str())});
        } else if (a == "--hs") o.hi_speed = std::clamp(std::atoi(next()), 10, 900);
        else if (a == "--effect") o.effect = std::clamp(std::atoi(next()), 0, 3);
        else if (a == "--seed") o.play.seed = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--timing") o.timing = std::clamp(std::atoi(next()), -199, 199);
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
    return true;  // 譜面を指定しなければ選曲画面
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
int simulate(const Chart& chart, int gauge, bool gas) {
    JudgeConfig cfg;
    cfg.gauge_type = gauge;
    cfg.gas = gas;
    cfg.autoplay = true;
    JudgeEngine j(chart, cfg);
    for (int64_t t = -1000000; t <= chart.end_us; t += 1000) j.advance(t);
    const PlayStats& s = j.stats();
    std::printf("ランプ %d / EX %d / MAX COMBO %d / BP %d / %d ノーツ\n", j.clear(), s.exscore(), s.max_combo, s.bp(),
                s.total_notes);
    return 0;
}

// 音の遅れを測る（開発用）。クリック音を「鳴らす」と命令した時刻から、オーディオ機器がその音を鳴らす時刻までを、
// WASAPI の IAudioClock（今どこまで鳴らしたかを時刻つきで返す）で求める。共有モードでも排他モードでも同じ物差しで測れる
int latency_test(bool exclusive, int buffer) {
    Mixer m;
    if (!m.open(0, buffer > 0 ? static_cast<uint32_t>(buffer) : 128, exclusive)) {
        std::printf("音声デバイスを開けない\n");
        return 1;
    }
    std::printf("モード: %s／%u Hz／周期 %u フレーム × %u／見積もりの遅延 %.1f ms\n", m.exclusive() ? "排他" : "共有",
                m.sample_rate(), m.period_frames(), m.periods(), m.output_latency_us() / 1000.0);
#ifdef _WIN32
    SDL_Delay(300);
    if (m.calibrate_latency()) std::printf("曲の時計に使う遅延（実測）: %.2f ms\n", m.output_latency_us() / 1000.0);
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    auto now_100ns = [&] {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1e7 / static_cast<double>(qf.QuadPart);
    };
    SDL_Delay(500);
    std::vector<double> ms;
    for (int i = 0; i < 40; ++i) {
        int64_t before = m.click_frame();
        double t_call = now_100ns();
        m.play_test_click();
        while (m.click_frame() == before) SDL_Delay(0);
        int64_t frame = m.click_frame();
        SDL_Delay(100);
        double played = 0;
        uint64_t qpc = 0;
        if (!m.device_position(played, qpc)) {
            std::printf("オーディオ機器の位置を取れない\n");
            return 1;
        }
        // クリックのフレームが鳴る時刻 = 位置を取った時刻 −（そこまでに鳴った時間 − クリックの位置）
        double click_sec = static_cast<double>(frame) / m.sample_rate();
        double t_play = static_cast<double>(qpc) - (played - click_sec) * 1e7;
        ms.push_back((t_play - t_call) / 1e4);
        SDL_Delay(150 + (i * 37) % 50);  // 周期とそろわないように少しずつずらす
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    double mean = sum / ms.size(), var = 0;
    for (double v : ms) var += (v - mean) * (v - mean);
    std::printf("実測の遅延（%zu 回）: 平均 %.2f ms／中央 %.2f ms／最小 %.2f ms／最大 %.2f ms／標準偏差 %.2f ms\n", ms.size(), mean,
                ms[ms.size() / 2], ms.front(), ms.back(), std::sqrt(var / ms.size()));
#else
    std::printf("実測は Windows（WASAPI）だけ\n");
#endif
    m.close();
    return 0;
}

// 起動の画面（LR2: Main.cpp の起動処理）。真っ黒な画面の左上に白い文字を足していく。
//   曲データベースの更新中は「Now Loading...」と読み込み中の譜面のパス（1件ごとに描き直す）、
//   難易度表の取得中は表ごとの状態を出し、最後に版の名前と「PUSH ANY KEY」を出して入力を待つ。押すと「READY」。
// 閉じられたら false
bool startup_screen(App& app, bool update) {
    // LR2（DxLib の printfDx）と同じく、拡大せずにウィンドウの画素にそのまま、ぼかさない 16px の文字で描く
    SDL_SetRenderLogicalPresentation(app.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    app.view_w = app.view_h = 0;  // 次の set_view で必ず設定し直す
    const float px = std::max(1.0f, SDL_GetWindowPixelDensity(app.window));
    const int font = static_cast<int>(16 * px);
    TextCache text(app.renderer);
    text.set_antialias(false);
#ifdef _WIN32
    // LR2（DxLib）の既定の文字と同じ MS ゴシック
    text.set_system_font("MS Gothic");
#endif
    const SDL_Color white{255, 255, 255, 255};
    std::vector<std::string> lines;  // 画面に残っていく行
    bool closed = false;
    const char* startup_shot = nullptr;
    auto draw = [&](const std::vector<std::string>& temp) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) closed = true;
        SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
        SDL_RenderClear(app.renderer);
        float y = 0;
        for (const std::string& l : lines) text.draw(0, y, font, white, l, 0, 0, 0), y += font;
        for (const std::string& l : temp) text.draw(0, y, font, white, l, 0, 0, 0), y += font;
        if (const char* shot = std::getenv("BMJ_LOADING_SHOT"); shot && !temp.empty()) {
            // 開発用：読み込み中の画面を1枚保存する
            static bool saved = false;
            if (!saved)
                if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf), saved = true;
        }
        if (startup_shot)
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, startup_shot), SDL_DestroySurface(sf);
        SDL_RenderPresent(app.renderer);
    };
    draw({});
    if (update) {
        SongDb db;
        if (db.open(song_db_path())) {
            if (std::getenv("BMJ_FORCE_RESCAN")) db.clear_charts();  // 開発用：全部読み直す
            int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
            ScanStats s = db.scan(app.cfg.song_dirs, threads, [&](int, int, const std::string& path) {
                if (!path.empty()) draw({"Now Loading...", path});
            });
            std::printf("曲データベースを更新した：%d 譜面（追加 %d／変更 %d／削除 %d／読めない %d）%.1f 秒\n", s.found,
                        s.added, s.updated, s.removed, s.failed, s.seconds);
        } else {
            std::printf("曲データベースを開けない: %s\n", song_db_path().c_str());
        }
        // 難易度表はまとめて並べて取りに行く（応答の無いサイトがあっても、待つのは1つぶんで済む）
        const std::vector<std::string>& urls = app.cfg.tables;
        std::vector<std::future<DifficultyTable>> jobs;
        for (const std::string& u : urls) jobs.push_back(std::async(std::launch::async, [u] { return update_table(table_cache_dir(), u); }));
        std::vector<std::optional<DifficultyTable>> got(jobs.size());
        for (int tick = 0;; ++tick) {
            bool all = true;
            std::vector<std::string> temp{"Loading difficulty tables:"};
            for (size_t i = 0; i < jobs.size(); ++i) {
                if (!got[i] && jobs[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready) got[i] = jobs[i].get();
                if (!got[i]) all = false;
                temp.push_back("[" + urls[i] + "]: " + (got[i] ? "done" : std::string(1 + tick / 10 % 3, '.')));
            }
            if (all || jobs.empty()) break;
            draw(temp);
            SDL_Delay(16);
        }
        for (size_t i = 0; i < got.size(); ++i) {
            const DifficultyTable& t = *got[i];
            std::string name = t.name.empty() ? urls[i] : t.name;
            std::string res = t.warning.empty() ? std::to_string(t.charts.size()) + " charts" : t.from_cache ? "failed (using cache)" : "failed";
            lines.push_back("[" + name + "]: " + res);
            std::printf("難易度表 %s：%zu 譜面%s\n", name.c_str(), t.charts.size(), t.warning.empty() ? "" : ("（" + t.warning + "）").c_str());
        }
    }
    lines.push_back("");
    lines.push_back("BMjoy version 0.1.0");
    lines.push_back("PUSH ANY KEY");
    startup_shot = std::getenv("BMJ_STARTUP_SHOT");  // 開発用：入力待ちの画面を1枚保存して終わる
    draw({});
    if (startup_shot) return false;
    // 何か押されるまで待つ（キーボード・コントローラーのボタン・マウス）。開発用の撮影中は待たない
    bool pushed = std::getenv("BMJ_SELECT_SHOT") != nullptr;
    while (!pushed && !closed) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) closed = true;
            if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                pushed = true;
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) SDL_OpenJoystick(e.jdevice.which);
        }
        SDL_Delay(5);
    }
    if (closed) return false;
    lines.push_back("READY");
    draw({});
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ログをファイルに流しても、落ちたり止まったりしたときに残るようにする
#if defined(_WIN32)
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // コマンドプロンプトから動かしたときに、日本語のログを読めるようにする
    SetConsoleOutputCP(CP_UTF8);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "使い方: bmjoy <譜面ファイル> [--auto] [--gauge groove|hard|death|easy|pattack|gattack] [--gas]\n"
                     "                [--hs 100] [--seed N] [--timing ms] [--lr2-root DIR] [--skin FILE]\n"
                     "                [--skin-opt 番号=選択] [--skin-res sd|hd|fhd] [--no-skin] [--fps N] [--vsync]\n"
                     "                [--simulate] [--info]\n"
                     "       bmjoy --keyconfig\n");
        return 2;
    }
    if (opt.latency_test) return latency_test(opt.exclusive, opt.buffer);
    if (!opt.table_urls.empty()) {
        std::string cache = opt.db_path.empty() ? "tables_test" : opt.db_path;
        for (const std::string& u : opt.table_urls) {
            DifficultyTable t = update_table(cache, u);
            std::printf("%s [%s] %zu 譜面、%zu レベル%s\n", t.name.empty() ? u.c_str() : t.name.c_str(), t.symbol.c_str(),
                        t.charts.size(), t.levels.size(), t.from_cache ? "（手元の版）" : "");
            if (!t.warning.empty()) std::printf("  %s\n", t.warning.c_str());
            if (!t.levels.empty()) std::printf("  レベル: %s 〜 %s\n", t.levels.front().c_str(), t.levels.back().c_str());
        }
        return 0;
    }
    if (!opt.scan_dirs.empty()) {
        SongDb db;
        std::string path = opt.db_path.empty() ? "songs_scan_test.db" : opt.db_path;
        if (!db.open(path)) return 1;
        int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
        ScanStats s = db.scan(opt.scan_dirs, threads);
        std::printf("%d 譜面（追加 %d／変更 %d／削除 %d／読めない %d）%.2f 秒（%d スレッド）\n", s.found, s.added, s.updated,
                    s.removed, s.failed, s.seconds, threads);
        return 0;
    }
    if (opt.info || opt.simulate) {
        BmsParseResult parsed = parse_chart_file(opt.chart_path, {opt.play.seed ? opt.play.seed : 1});
        if (!parsed.ok) {
            std::fprintf(stderr, "読み込めない: %s\n", parsed.error.c_str());
            return 1;
        }
        if (opt.info) print_info(parsed.chart, parsed);
        else simulate(parsed.chart, opt.gauge >= 0 ? opt.gauge : kGroove, opt.gas);
        return 0;
    }

    std::printf("SDLを初期化しています（映像）\n");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDLの初期化に失敗: %s\n", SDL_GetError());
        return 1;
    }
    int rc = 0;
    {
        App app;
        app.cfg_path = config_path();
        if (app.cfg.load(app.cfg_path)) std::printf("設定: %s\n", app.cfg_path.c_str());
        // コマンドラインの指定は設定に上書きし、終了時に保存する
        AppConfig& c = app.cfg;
        std::error_code ec;
        if (!opt.lr2_root.empty()) c.lr2_root = std::filesystem::absolute(opt.lr2_root, ec).string();
        if (!opt.skin.empty()) c.play_skin = std::filesystem::absolute(opt.skin, ec).string();
        if (!opt.result_skin.empty()) c.result_skin = std::filesystem::absolute(opt.result_skin, ec).string();
        if (!opt.skin_res.empty()) c.skin_res = opt.skin_res;
        if (opt.gauge >= 0) c.gauge = opt.gauge;
        if (opt.gas) c.gas = true;
        if (opt.hi_speed > 0) c.hi_speed = opt.hi_speed;
        if (opt.effect >= 0) c.effect = opt.effect;
        if (opt.timing != 1000) c.judge_timing = opt.timing;
        if (opt.fps != -1000) c.fps = opt.fps;
        if (opt.vsync) c.vsync = true;
        for (const std::string& d : opt.add_song_dirs) {
            std::string a = std::filesystem::absolute(d, ec).string();
            if (std::find(c.song_dirs.begin(), c.song_dirs.end(), a) == c.song_dirs.end()) c.song_dirs.push_back(a);
        }
        for (const std::string& t : opt.add_tables)
            if (std::find(c.tables.begin(), c.tables.end(), t) == c.tables.end()) c.tables.push_back(t);
        joystick_init().start(c.joystick_directinput);
        raw_hid().start();
        if (!app.init(argv[0])) return 1;
        // LR2 と同じ起動の画面。曲データベースと難易度表の更新もここで行う（設定で「手動」なら選曲画面の F5 だけ）
        if (opt.chart_path.empty() && !opt.keyconfig) {
            bool update = c.db_update == 0;
            if (!startup_screen(app, update)) return 0;
            app.updated_at_start = update;
        }
        if (opt.no_skin) app.lr2_root.clear();
        if (!opt.skin_opts.empty()) {
            // カスタマイズの指定は、使うスキンのパスに結びつけて保存する
            std::string skin_path = c.play_skin.empty()
                                        ? resolve_lr2_path(app.lr2_root, "LR2files/Theme/LR2/Play/play_7.lr2skin", false)
                                        : c.play_skin;
            auto& sel = c.skin_custom[skin_path];
            SkinHeader h;
            if (read_skin_header(app.lr2_root, skin_path, h) && sel.size() < h.customs.size()) {
                size_t old = sel.size();
                sel.resize(h.customs.size());
                for (size_t i = old; i < sel.size(); ++i) sel[i] = h.customs[i].selected;
            }
            for (auto [idx, choice] : opt.skin_opts)
                if (idx >= 0 && idx < static_cast<int>(sel.size())) sel[idx] = choice;
        }

        if (!opt.import_lr2.empty()) {
            std::string err;
            int n = app.db.import_lr2_scores(opt.import_lr2, &err);
            if (n < 0) std::printf("LR2のスコアを取り込めない: %s\n", err.c_str());
            else std::printf("LR2のスコアを %d 件取り込んだ\n", n);
        }
        if (opt.keyconfig) {
            run_keyconfig(app.window, app.renderer, app.keycfg, keyconfig_path());
        } else if (opt.chart_path.empty()) {
            run_select(app);
        } else {
            run_play(app, opt.chart_path, opt.play);
        }
        if (!opt.no_skin) app.save_config();
    }
    raw_hid().stop();
    if (!joystick_init().ready()) {
        // 準備中（Windows の DirectInput の列挙中）なら、待たずにそのまま終わる。設定やスコアは保存済み
        std::fflush(stdout);
        std::_Exit(rc);
    }
    joystick_init().join();
    SDL_Quit();
    return rc;
}
