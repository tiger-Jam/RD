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
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "audio/mixer.h"
#include "app/play_session.h"
#include "app/select_scene.h"
#include "app/skin_catalog.h"
#include "chart/bms_parser.h"
#include "chart/chart_loader.h"
#include "db/difficulty_table.h"
#include "app/lr2_import.h"
#include "db/song_db.h"
#include "game/judge.h"
#include "input/joystick_init.h"
#include "input/midi.h"
#include "input/win_rawhid.h"
#include "render/frame_hooks.h"
#include "render/keyconfig_scene.h"
#include "render/text_cache.h"
#include "render/skin_play.h"
#include "skin/lr2_skin.h"
#include "skin/skin_parts.h"
#include "util/data_dir.h"
#include "util/dev.h"
#include "util/startup_clock.h"

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
    std::string import_lr2_dir;  // LR2 のフォルダ（曲フォルダの登録・曲の情報・スコアをまとめて引き継いで終わる）
    bool copy_assets = false;
    std::string import_lr2_courses;
    // 持っていない譜面の一覧（難易度表の名前・記号・URL のどれか）。--level で絞る（何度でも）。出したら終わる
    std::string list_missing;
    bool list_all = false;  // --list-charts：持っている譜面も出す
    std::vector<std::string> levels;
    std::string skin_parts;  // 開発用：プレースキン（--skin で替えられる）を部品ごとに切り出して、このフォルダに書いて終わる
    bool list_owned = false;  // 持っている譜面の md5 とフォルダを 1 行ずつ出して終わる
    // 外の道具（曲を集める道具）から使う：登録した難易度表を取り直す（その日の 2 回目からは手元の版）／登録した曲フォルダを全部読み直す
    bool update_tables = false, rescan = false;
    std::string tables_file;  // 登録した表に足して使う表の URL の一覧（1 行に 1 つ。収集だけに使い、選曲画面には出さない）
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
        else if (a == "--replay") o.play.replay = true;  // 開発用：保存してあるリプレイを再生する
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
        else if (a == "--import-lr2-dir") o.import_lr2_dir = next();
        else if (a == "--copy-lr2-assets") o.copy_assets = true;  // 今の素材のフォルダ（LR2）の素材を、実行ファイルの横に写して付け替える
        else if (a == "--import-lr2-courses") o.import_lr2_courses = next();
        else if (a == "--list-missing") o.list_missing = next();
        else if (a == "--list-charts") o.list_missing = next(), o.list_all = true;
        else if (a == "--skin-parts") o.skin_parts = next();
        else if (a == "--list-owned") o.list_owned = true;
        else if (a == "--update-tables") o.update_tables = true;
        else if (a == "--rescan") o.rescan = true;
        else if (a == "--tables-file") o.tables_file = next();
        else if (a == "--level") o.levels.push_back(next());  // LR2 の song.db の段位・コースだけを写す
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
// before_wait は「PUSH ANY KEY」を出す直前に呼ぶ（選曲画面の下ごしらえを始め、押されるのを待つ間に済ませる）
// only が空でなければ、曲フォルダのうちそのフォルダだけを読む（「手動」のときの、まだ読んでいない曲フォルダ）
bool startup_screen(App& app, bool update, const std::function<void()>& before_wait = {},
                    const std::vector<std::string>& only = {}) {
    // LR2（DxLib の printfDx）と同じく、拡大せずにウィンドウの画素にそのまま、ぼかさない 16px の文字で描く
    frame_target_set(app.renderer, 0, 0);
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
        if (const char* shot = dev_env("BMJ_LOADING_SHOT"); shot && !temp.empty()) {
            // 開発用：読み込み中の画面を1枚保存する
            static bool saved = false;
            if (!saved)
                if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf), saved = true;
        }
        if (startup_shot)
            if (SDL_Surface* sf = SDL_RenderReadPixels(app.renderer, nullptr)) SDL_SaveBMP(sf, startup_shot), SDL_DestroySurface(sf);
        frame_present(app.renderer);
    };
    draw({});
    if (update) {
        SongDb db;
        if (db.open(song_db_path())) {
            if (dev_env("BMJ_FORCE_RESCAN")) db.clear_charts();  // 開発用：全部読み直す
            int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
            // 曲フォルダの確かめは裏で行い、その間も画面を描き直して応答する（HDD の中身の情報が OS に残っていないと
            // 数分かかることがあり、その間黙っていると Windows に「応答なし」として閉じられる）
            std::mutex scan_mu;
            std::string scan_path;
            auto scan_job = std::async(std::launch::async, [&] {
                auto progress = [&](int, int, const std::string& path) {
                    std::lock_guard<std::mutex> lock(scan_mu);
                    if (!path.empty()) scan_path = path;
                };
                if (only.empty()) return db.scan(app.cfg.song_dirs, threads, progress);
                ScanStats total;
                for (const std::string& d : only) {
                    ScanStats s = db.scan(app.cfg.song_dirs, threads, progress, d);
                    total.found += s.found, total.added += s.added, total.updated += s.updated, total.removed += s.removed;
                    total.failed += s.failed, total.seconds += s.seconds;
                }
                return total;
            });
            while (scan_job.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
                std::string p;
                {
                    std::lock_guard<std::mutex> lock(scan_mu);
                    p = scan_path;
                }
                draw({"Now Loading...", p});
            }
            ScanStats s = scan_job.get();
            std::printf("曲データベースを更新した：%d 譜面（追加 %d／変更 %d／削除 %d／読めない %d）%.1f 秒\n", s.found,
                        s.added, s.updated, s.removed, s.failed, s.seconds);
            // LR2 から引き継いだ仮の譜面（中身をまだ読んでいないもの）も、ここで全部読む（LR2 と同じく起動の画面で済ませる）
            const int total = db.provisional_count();
            if (total > 0) {
                auto t0 = std::chrono::steady_clock::now();
                const int fill_threads = std::max(2, threads / 2);
                int done = 0;
                while (!closed) {
                    int n = db.fill_provisional(200, fill_threads);
                    done += n;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%d / %d", std::min(done, total), total);
                    draw({"Now Loading...", buf});
                    if (n == 0 || done >= total) break;
                }
                double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("引き継いだ譜面を読んだ：%d / %d 譜面 %.1f 秒\n", done, total, sec);
            }
        } else {
            std::printf("曲データベースを開けない: %s\n", song_db_path().c_str());
        }
    }
    startup_mark("曲データベースの更新を終えた");
    {
        // 難易度表は、曲フォルダの更新を手動にしていても起動のたびに確かめる（取りに行くのはその日の最初の 1 回だけ）。
        // まとめて並べて取りに行く。応答の無いサイトがあっても、起動の画面で待つのは 2 秒まで（残りは裏で続け、
        // 間に合わなかった表が新しくなっていたら、選曲画面が一覧を作り直す）
        const std::vector<std::string>& urls = app.cfg.tables;
        auto tu = std::make_shared<TableUpdates>();
        tu->got.resize(urls.size());
        tu->remaining = static_cast<int>(urls.size());
        app.table_updates = tu;
        for (size_t i = 0; i < urls.size(); ++i)
            std::thread([tu, i, u = urls[i]] {
                DifficultyTable t = update_table(table_cache_dir(), u);
                const bool changed = !t.from_cache && t.warning.empty();
                {
                    std::lock_guard<std::mutex> lock(tu->mu);
                    tu->got[i] = std::move(t);
                }
                if (changed && tu->cut) tu->late_changed = true;
                --tu->remaining;
            }).detach();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (int tick = 0; tu->remaining > 0 && std::chrono::steady_clock::now() < deadline; ++tick) {
            std::vector<std::string> temp{"Loading difficulty tables:"};
            {
                std::lock_guard<std::mutex> lock(tu->mu);
                for (size_t i = 0; i < urls.size(); ++i)
                    temp.push_back("[" + urls[i] + "]: " + (tu->got[i] ? "done" : std::string(1 + tick / 10 % 3, '.')));
            }
            draw(temp);
            SDL_Delay(16);
        }
        tu->cut = true;
        std::lock_guard<std::mutex> lock(tu->mu);
        for (size_t i = 0; i < urls.size(); ++i) {
            if (!tu->got[i]) {
                lines.push_back("[" + urls[i] + "]: checking in background");
                std::printf("難易度表 %s：裏で確かめている\n", urls[i].c_str());
                continue;
            }
            const DifficultyTable& t = *tu->got[i];
            std::string name = t.name.empty() ? urls[i] : t.name;
            // 毎回、表ごとにどう読んだかを出す（今日取りに行った／今日すでに取った手元の版／取れずに手元の版／取れない）
            const std::string n = std::to_string(t.charts.size()) + " charts";
            std::string res = !t.warning.empty() ? (t.from_cache ? "update failed, " + n + " (local copy)" : "failed")
                              : t.from_cache   ? n + " (checked today)"
                                               : n + " (updated)";
            lines.push_back("[" + name + "]: " + res);
            std::printf("難易度表 %s：%zu 譜面%s\n", name.c_str(), t.charts.size(), t.warning.empty() ? "" : ("（" + t.warning + "）").c_str());
        }
    }
    startup_mark("難易度表を読んだ");
    if (before_wait) before_wait();
    lines.push_back("");
    lines.push_back("BMjoy version 0.1.0");
    lines.push_back("PUSH ANY KEY");
    startup_shot = dev_env("BMJ_STARTUP_SHOT");  // 開発用：入力待ちの画面を1枚保存して終わる
    draw({});
    if (startup_shot) return false;
    // 何か押されるまで待つ（キーボード・コントローラーのボタン・マウス）。開発用の撮影中は待たない
    bool pushed = dev_env("BMJ_SELECT_SHOT") != nullptr || dev_env("BMJ_SCROLL_TEST") != nullptr || dev_env("BMJ_OPEN_TEST") != nullptr;
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
    startup_mark("キーが押された");
    lines.push_back("READY");
    draw({});
    return true;
}

}  // namespace

// 難易度表の譜面のうち、曲データベースに無いもの（持っていないもの）を 1 行ずつ出す：md5、レベル、曲名、アーティスト、表の名前、
// 本体の URL、差分の URL（表に書いてあれば）。
// 表は手元の版（BMjoy が起動のときに取ったもの）を使う。which は表の名前・記号・URL のどれか（名前は大文字小文字を区別せず、一部でよい）。"*" なら登録した表を全部。
// 外の道具（曲を取ってくる道具など）から使う
// 登録した表の URL に、一覧のファイル（あれば）の URL を足したもの（重なりは 1 つ）
std::vector<std::string> table_urls_with(const AppConfig& cfg, const std::string& file) {
    std::vector<std::string> urls = cfg.tables;
    if (file.empty()) return urls;
    std::ifstream f(std::filesystem::u8path(file));
    std::unordered_set<std::string> have(urls.begin(), urls.end());
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (!line.empty() && line[0] != '#' && have.insert(line).second) urls.push_back(line);
    }
    return urls;
}

// 難易度表の、持っていない譜面を 1 行ずつ出す（md5・記号とレベル・曲名・作者・表の名前・本体の URL・差分の URL）。
// all なら持っている譜面も出し、行の終わりに持っているか（1 / 0）と表の URL を足す
// プレースキンを部品ごとに切り出す（設定は読むだけで書かない）
int export_parts(const char* argv0, const Options& opt) {
    AppConfig cfg;
    cfg.load(config_path());
    std::error_code ec;
    const std::string root = find_lr2_root(opt.lr2_root.empty() ? cfg.lr2_root : opt.lr2_root, argv0);
    const std::string path = opt.skin.empty() ? cfg.play_skin : std::filesystem::absolute(opt.skin, ec).string();
    SkinHeader header;
    if (root.empty() || path.empty() || !read_skin_header(root, path, header)) {
        std::fprintf(stderr, "スキンを読めない: %s\n", path.c_str());
        return 1;
    }
    apply_skin_custom(cfg, path, header);
    Lr2Skin skin;
    std::string err;
    if (!load_lr2_skin(root, path, header, PlayContext{}.load_options(), skin, err)) {
        std::fprintf(stderr, "スキンを読めない: %s\n", err.c_str());
        return 1;
    }
    skin.path = path;
    if (!header.has_resolution) guess_resolution(skin);
    if (!export_skin_parts(skin, opt.skin_parts, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    return 0;
}

int list_missing(const std::string& which, const std::vector<std::string>& levels, const std::string& tables_file, bool all = false) {
    AppConfig cfg;
    migrate_old_data();
    cfg.load(config_path());
    SongDb db;
    if (!db.open(song_db_path())) {
        std::fprintf(stderr, "曲データベースを開けない: %s\n", song_db_path().c_str());
        return 1;
    }
    std::unordered_set<std::string> owned;
    for (const ChartRecord& c : db.all_charts()) owned.insert(c.md5);
    auto low = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string want = low(which);
    int tables = 0, missing = 0;
    for (const std::string& url : table_urls_with(cfg, tables_file)) {
        DifficultyTable t;
        if (!load_cached_table(table_cache_dir(), url, t)) continue;
        if (which != "*" && !(low(url).find(want) != std::string::npos || low(t.name).find(want) != std::string::npos ||
              (!t.symbol.empty() && t.symbol == which)))
            continue;
        ++tables;
        std::unordered_set<std::string> seen;
        for (const TableChart& c : t.charts) {
            const bool have = owned.count(low(c.md5)) > 0;
            if (c.md5.size() != 32 || (have && !all) || !seen.insert(low(c.md5)).second) continue;
            if (!levels.empty() && std::find(levels.begin(), levels.end(), c.level) == levels.end()) continue;
            // タブと改行は区切りなので、曲名などに混ざっていれば空白にする
            auto field = [](std::string v) {
                for (char& ch : v)
                    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
                return v;
            };
            std::printf("%s\t%s%s\t%s\t%s\t%s\t%s\t%s", low(c.md5).c_str(), field(t.symbol).c_str(), field(c.level).c_str(),
                        field(c.title).c_str(), field(c.artist).c_str(), field(t.name).c_str(), field(c.url).c_str(), field(c.url_diff).c_str());
            if (all) std::printf("\t%d\t%s", have ? 1 : 0, field(url).c_str());
            std::printf("\n");
            if (!have) ++missing;
        }
    }
    std::fprintf(stderr, "難易度表 %d 個、持っていない譜面 %d 件\n", tables, missing);
    return tables > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    startup_clock_start();
    // ログをファイルに流しても、落ちたり止まったりしたときに残るようにする
#if defined(_WIN32)
    {
        // ダブルクリックで起動したとき（出力の行き先が無いとき）は、ログを LR2files/log.txt に残す。前の 1 回分は log_old.txt。
        // 別の PC やテスターの環境で、遊べなかった・止まったときの理由を後から見られるように
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) {
            const std::filesystem::path dir = std::filesystem::u8path(data_dir());
            std::error_code ec;
            std::filesystem::rename(dir / "log.txt", dir / "log_old.txt", ec);
            const std::wstring log = (dir / "log.txt").wstring();
            FILE* f = nullptr;
            // stderr は stdout と同じ書き先にする（別々に開くと、片方の書いた行をもう片方が上書きする）
            if (_wfreopen_s(&f, log.c_str(), L"w", stdout) == 0) _dup2(_fileno(stdout), _fileno(stderr));
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // コマンドプロンプトから動かしたときに、日本語のログを読めるようにする
    SetConsoleOutputCP(CP_UTF8);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    Options opt;
#ifdef _WIN32
    // Windows の argv は ANSI（Shift_JIS）なので、日本語の引数（曲のパス・難易度表の名前）が化ける。UTF-16 の引数から作り直す
    std::vector<std::string> wargs;
    std::vector<char*> wargv;
    {
        int wn = 0;
        if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &wn)) {
            for (int i = 0; i < wn; ++i) {
                int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
                std::string u(len > 0 ? len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, u.data(), len, nullptr, nullptr);
                wargs.push_back(std::move(u));
            }
            LocalFree(w);
            for (std::string& a : wargs) wargv.push_back(a.data());
            wargv.push_back(nullptr);
            argc = wn, argv = wargv.data();
        }
    }
#endif
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
    if (!opt.list_missing.empty()) return list_missing(opt.list_missing, opt.levels, opt.tables_file, opt.list_all);
    if (!opt.skin_parts.empty()) return export_parts(argv[0], opt);
    if (opt.update_tables || opt.rescan) {
        AppConfig cfg;
        migrate_old_data();
        cfg.load(config_path());
        int rc = 0;
        if (opt.update_tables) {
            // 表が何百あっても、同時に取りに行くのは 16 個まで
            const std::vector<std::string> urls = table_urls_with(cfg, opt.tables_file);
            for (size_t from = 0; from < urls.size(); from += 16) {
                std::vector<std::future<DifficultyTable>> jobs;
                for (size_t i = from; i < std::min(urls.size(), from + 16); ++i)
                    jobs.push_back(std::async(std::launch::async, [u = urls[i]] { return update_table(table_cache_dir(), u); }));
                for (size_t i = 0; i < jobs.size(); ++i) {
                    DifficultyTable t = jobs[i].get();
                    std::printf("難易度表 %s：%zu 譜面%s\n", t.name.empty() ? urls[from + i].c_str() : t.name.c_str(), t.charts.size(),
                                t.warning.empty() ? "" : ("（" + t.warning + "）").c_str());
                }
            }
        }
        if (opt.rescan) {
            SongDb db;
            if (!db.open(song_db_path())) {
                std::fprintf(stderr, "曲データベースを開けない: %s\n", song_db_path().c_str());
                return 1;
            }
            // 見つからない曲フォルダ（外付けのドライブが外れているなど）があれば読まない。そのまま読むと、その下の譜面が全部消える
            for (const std::string& d : cfg.song_dirs) {
                std::error_code ec;
                if (!std::filesystem::is_directory(std::filesystem::u8path(d), ec)) {
                    std::fprintf(stderr, "曲フォルダが見つからないので読み直さない: %s\n", d.c_str());
                    return 1;
                }
            }
            int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
            ScanStats s = db.scan(cfg.song_dirs, threads);
            std::printf("曲フォルダを読み直した：%d 譜面（追加 %d／変更 %d／削除 %d／読めない %d）%.1f 秒\n", s.found, s.added, s.updated,
                        s.removed, s.failed, s.seconds);
            if (cfg.song_dirs.empty()) rc = 1;
        }
        return rc;
    }
    if (opt.list_owned) {
        // 持っている譜面の md5 と、それがあるフォルダ（外の道具が、差分を本体のフォルダへ入れるのに使う）
        SongDb db;
        if (!db.open(song_db_path())) return 1;
        for (const ChartRecord& c : db.all_charts()) std::printf("%s\t%s\n", c.md5.c_str(), c.folder.c_str());
        return 0;
    }
    if (!opt.table_urls.empty()) {
        std::string cache = opt.db_path.empty() ? "tables_test" : opt.db_path;
        for (const std::string& u : opt.table_urls) {
            DifficultyTable t = update_table(cache, u, true);
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
        migrate_old_data();  // 前の置き場（OS の設定フォルダ）の設定と記録を、実行ファイルの横の LR2files へ
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
        startup_mark("SDL の準備が終わった");
        joystick_init().start(c.joystick_directinput);
        raw_hid().start();
        if (!app.init(argv[0])) return 1;
        startup_mark("ウィンドウ・音声・曲データベースの準備が終わった");
        midi_input().start();  // SDL のイベントで送るので、SDL の準備の後に開く
        if (!opt.import_lr2_courses.empty()) {
            int n = import_lr2_courses(opt.import_lr2_courses);
            if (n < 0) std::printf("LR2 の段位とコースを読めない: %s\n", opt.import_lr2_courses.c_str());
            else std::printf("LR2 の段位とコースを %d 件引き継いだ\n", n);
            return n < 0 ? 1 : 0;
        }
        if (opt.copy_assets) {
            const char* exe_dir = SDL_GetBasePath();
            if (c.lr2_root.empty()) c.lr2_root = app.lr2_root;
            AssetCopyResult r = copy_lr2_assets(c, exe_dir ? exe_dir : "", [](const std::string& s) { std::printf("%s\n", s.c_str()); });
            if (!r.ok) {
                std::printf("素材を写せない: %s\n", r.error.c_str());
                return 1;
            }
            std::printf("素材を写した：%d ファイル（%.1f MB）、写せなかったもの %d。素材のフォルダ：%s\n", r.files, r.bytes / 1048576.0, r.failed,
                        c.lr2_root.c_str());
            app.save_config();
            return 0;
        }
        if (!opt.import_lr2_dir.empty()) {
            auto t0 = std::chrono::steady_clock::now();
            Lr2ImportResult r = import_from_lr2(opt.import_lr2_dir, c, app.db, [](const std::string& s) { std::printf("%s\n", s.c_str()); },
                                              app.lr2_root);
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!r.ok) {
                std::printf("LR2 から引き継げない: %s\n", r.error.c_str());
                return 1;
            }
            std::printf("LR2 から引き継いだ：プレイヤー %s、曲フォルダ %d、7 鍵の譜面 %d、スコア %d、難易度表 %d、スキン %d（%.1f 秒）\n",
                        r.player.c_str(), r.song_dirs, r.charts, r.scores, r.tables, r.skins, sec);
            for (const std::string& d : c.song_dirs) std::printf("  曲フォルダ: %s\n", d.c_str());
            app.save_config();
            return 0;
        }
        // LR2 と同じ起動の画面。曲データベースの更新（設定で「手動」なら選曲画面の F8 だけ）と難易度表の更新もここで行う
        std::future<std::shared_ptr<SelectPrep>> select_prep;  // 選曲画面の下ごしらえ（起動の画面の間に裏で作る）
        if (opt.chart_path.empty() && !opt.keyconfig) {
            if (c.watch_folders) app.watcher.start(c.song_dirs);
            startup_mark("曲フォルダの見張りを始めた");
            bool update = c.db_update == 0;
            std::vector<std::string> unread;
            if (!update) {
                // 「手動」でも、まだ一度も読んでいない曲フォルダ（新しく登録した・別の PC へ持ってきた・曲データベースを
                // 再作成した）は読む。読まないと、その曲フォルダは空のままになる。読むのはそのフォルダだけ
                for (const std::string& d : c.song_dirs) {
                    std::error_code ec;
                    // 先に曲データベースで確かめる（読んであるフォルダのためにドライブを見に行くと、止まっている HDD が回るまで待たされる）
                    if (!app.db.has_charts_under(d) && std::filesystem::is_directory(std::filesystem::u8path(d), ec)) {
                        std::printf("まだ読んでいない曲フォルダを読む: %s\n", d.c_str());
                        unread.push_back(d);
                    }
                }
                update = !unread.empty();
                startup_mark("まだ読んでいない曲フォルダを確かめた");
            }
            // 「PUSH ANY KEY」を待っている間に、選曲画面の一覧を裏で作り、選曲スキンを読んでおく（READY のあと待たせない）
            auto prepare = [&] {
                select_prep = std::async(std::launch::async, [&app] {
                    auto prep = prepare_select(app);
                    startup_mark("選曲画面の一覧ができた（裏）");
                    return prep;
                });
                if (!opt.no_skin && opt.skin_opts.empty()) preload_select_skin(app);
                startup_mark("選曲スキンを読んだ");
            };
            if (!startup_screen(app, update, prepare, unread)) {
                if (select_prep.valid()) select_prep.wait();
                return 0;
            }
            app.updated_at_start = update;
        }
        if (opt.no_skin) app.lr2_root.clear();
        if (!opt.skin_opts.empty()) {
            // カスタマイズの指定は、使うスキンのパスに結びつけて保存する
            std::string skin_path = c.play_skin.empty()
                                        ? resolve_lr2_path(app.lr2_root, "LR2files/Theme/LR2/Play/play_7.lr2skin", false)
                                        : c.play_skin;
            // 指定は選択肢の番号。#CUSTOMFILE は名前にして残す
            SkinHeader h;
            if (read_skin_header(app.lr2_root, skin_path, h)) {
                apply_skin_custom(c, skin_path, h);
                for (auto [idx, choice] : opt.skin_opts)
                    if (idx >= 0 && idx < static_cast<int>(h.customs.size()) && choice >= 0 &&
                        choice < static_cast<int>(h.customs[idx].labels.size()))
                        h.customs[idx].selected = choice;
                c.skin_custom[skin_path] = skin_custom_choices(h);
            }
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
            app.scene_end_ns = SDL_GetTicksNS();
            auto prep = select_prep.valid() ? select_prep.get() : nullptr;
            startup_mark("選曲画面に入る");
            run_select(app, prep);
        } else {
            // 開発用：BMJ_CHART_DELAY のときは、選曲画面から遊ぶのと同じく曲データベースの譜面の情報を渡す
            std::vector<ChartRecord> recs;
            if (dev_env("BMJ_CHART_DELAY")) {
                std::error_code ec;
                const std::string abs = std::filesystem::absolute(opt.chart_path, ec).lexically_normal().string();
                recs = app.db.charts_in_folder(std::filesystem::path(abs).parent_path().string());
                for (const ChartRecord& c : recs)
                    if (c.path == abs) opt.play.record = &c;
            }
            run_play(app, opt.chart_path, opt.play);
        }
        if (!opt.no_skin) app.save_config();
    }
    raw_hid().stop();
    midi_input().stop();
    if (!joystick_init().ready()) {
        // 準備中（Windows の DirectInput の列挙中）なら、待たずにそのまま終わる。設定やスコアは保存済み
        std::fflush(stdout);
        std::_Exit(rc);
    }
    joystick_init().join();
    SDL_Quit();
    return rc;
}
