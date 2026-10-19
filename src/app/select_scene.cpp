#include "app/select_scene.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <list>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app/play_session.h"
#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "app/select_context.h"
#include "app/decide_scene.h"
#include "app/skin_select_scene.h"
#include "util/text.h"
#include "audio/system_sound.h"
#include "db/difficulty_table.h"
#include "game/gauge.h"
#include "input/input.h"
#include "input/joystick_init.h"
#include "input/win_rawhid.h"
#include "render/keyconfig_scene.h"
#include "render/text_cache.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// beatoraja と同じ並びのソートと鍵盤数（BMjoy の拡張）
constexpr int kSortCount = 9, kModeCount = 8;
const char* const kSortNames[kSortCount] = {"OFF", "TITLE", "ARTIST", "BPM", "LENGTH", "LEVEL", "CLEAR", "SCORE", "MISSCOUNT"};
const char* const kModeNames[kModeCount] = {"ALL", "7KEY", "14KEY", "9KEY", "5KEY", "10KEY", "SINGLE", "DOUBLE"};
const bool kModeShows7[kModeCount] = {true, true, false, false, false, false, true, false};

struct Entry {
    enum class Kind { Folder, Chart, Missing, Course } kind = Kind::Folder;
    std::string label, sub, badge;  // badge はレベルの表示（例：sl5）
    const ChartRecord* chart = nullptr;
    TableChart missing;
    std::shared_ptr<Course> course;  // コースのとき
    bool course_folder = false;      // コースを集めたフォルダ（バーの種類 6）
    std::vector<Entry> children;
    int owned = 0, total = 0;  // フォルダの中で持っている譜面の数
};

// 曲の一覧（裏で読み直して、できたら差し替える）
struct Library {
    std::vector<ChartRecord> charts;
    std::vector<DifficultyTable> tables;
    std::unordered_map<std::string, ScoreRecord> scores;  // sha256 → スコア
};

SDL_Color lamp_color(int lamp) {
    switch (lamp) {
    case 1: return {150, 30, 30, 255};    // FAILED
    case 2: return {90, 210, 90, 255};    // EASY
    case 3: return {80, 160, 255, 255};   // NORMAL
    case 4: return {240, 240, 240, 255};  // HARD
    case 5: return {255, 210, 60, 255};   // FULL COMBO
    case 6: return {255, 255, 200, 255};  // PERFECT
    default: return {60, 60, 70, 255};    // NO PLAY
    }
}

const char* lamp_name(int lamp) {
    static const char* names[] = {"NO PLAY", "FAILED", "EASY CLEAR", "CLEAR", "HARD CLEAR", "FULL COMBO", "PERFECT"};
    return lamp >= 0 && lamp <= 6 ? names[lamp] : "?";
}

// 一覧の木を作る：難易度表（レベル別）と曲フォルダ
std::vector<Entry> build_tree(const Library& lib) {
    std::unordered_map<std::string, const ChartRecord*> by_md5, by_sha256;
    for (const ChartRecord& c : lib.charts) by_md5.emplace(c.md5, &c), by_sha256.emplace(c.sha256, &c);
    auto chart_entry = [&](const ChartRecord* c, const std::string& badge) {
        Entry e;
        e.kind = Entry::Kind::Chart;
        e.chart = c;
        e.label = c->title + (c->subtitle.empty() ? "" : " " + c->subtitle);
        e.sub = c->artist;
        e.badge = badge;
        return e;
    };
    std::vector<Entry> root;
    for (const DifficultyTable& t : lib.tables) {
        Entry tf;
        tf.label = t.name.empty() ? t.url : t.name;
        tf.sub = t.warning.empty() ? (t.from_cache ? "手元の版" : "") : t.warning;
        for (const std::string& lv : t.levels) {
            Entry lf;
            lf.badge = t.symbol + lv;
            lf.label = t.symbol + lv;
            for (const TableChart& tc : t.charts) {
                if (tc.level != lv) continue;
                auto it = by_md5.find(tc.md5);
                if (it != by_md5.end()) {
                    lf.children.push_back(chart_entry(it->second, lf.badge));
                    lf.owned++;
                } else {
                    Entry m;
                    m.kind = Entry::Kind::Missing;
                    m.missing = tc;
                    m.label = tc.title;
                    m.sub = tc.artist;
                    m.badge = lf.badge;
                    lf.children.push_back(m);
                }
                lf.total++;
            }
            tf.owned += lf.owned;
            tf.total += lf.total;
            tf.children.push_back(std::move(lf));
        }
        root.push_back(std::move(tf));
    }
    // コースのフォルダ（LR2 と同じく一番上の階層に、COURSE と 段位認定 を必ず出す）。
    // 中身は難易度表に書かれたコース。段位の制約があれば段位認定、無ければ COURSE に入れる
    Entry expert, grade;
    expert.label = "COURSE", grade.label = "段位認定";
    expert.course_folder = grade.course_folder = true;
    for (const DifficultyTable& t : lib.tables)
        for (const TableCourse& tc : t.courses) {
            Entry ce;
            ce.kind = Entry::Kind::Course;
            ce.course = std::make_shared<Course>(make_table_course(tc, by_md5, by_sha256));
            ce.label = ce.course->name;
            Entry& f = ce.course->type == 2 ? grade : expert;
            f.owned += ce.course->playable();
            f.total++;
            f.children.push_back(std::move(ce));
        }
    root.push_back(std::move(expert));
    root.push_back(std::move(grade));
    // 曲フォルダ：実際のフォルダごとにまとめる
    Entry songs;
    songs.label = "曲フォルダ";
    std::map<std::string, std::vector<const ChartRecord*>> by_folder;
    for (const ChartRecord& c : lib.charts) by_folder[c.folder].push_back(&c);
    for (auto& [folder, list] : by_folder) {
        Entry f;
        f.label = fs::path(folder).filename().string();
        std::sort(list.begin(), list.end(), [](const ChartRecord* a, const ChartRecord* b) {
            return a->notes < b->notes;
        });
        f.sub = list.empty() ? "" : list.front()->title;
        for (const ChartRecord* c : list) {
            f.children.push_back(chart_entry(c, std::to_string(c->playlevel)));
            f.owned++;
            f.total++;
        }
        songs.owned += f.owned;
        songs.total += f.total;
        songs.children.push_back(std::move(f));
    }
    root.push_back(std::move(songs));
    return root;
}


// プレイオプションのパネルを開いている間の鍵盤（LR2: Songselect.cpp:1350 SetPlayOption）。
//   1=鍵盤数の絞り込み 2=RANDOM 3=BATTLE 4=GAUGE 5=HS- 7=HS+ 6=ASSIST 6+7=HIDDEN/SUDDEN 5+7=HS-FIX
// BMjoy にまだ無いもの（鍵盤数の絞り込み・BATTLE）は何もしない。変えたら true
bool play_option_keys(AppConfig& cfg, const InputMapper& input, const std::vector<ButtonInput>& buttons) {
    bool pressed[8] = {};
    for (const ButtonInput& b : buttons)
        if (b.down && b.button >= kKey1 && b.button <= kKey7) pressed[b.button - kKey1 + 1] = true;
    // 前から押し続けている（LR2 の入力状態 2）
    auto held = [&](int k) { return input.button_held(kKey1 + k - 1) && !pressed[k]; };
    auto down = [&](int k) { return input.button_held(kKey1 + k - 1) || pressed[k]; };
    auto loop = [](int lo, int hi, int add, int& v) {
        v += add;
        if (v > hi) v = lo;
        else if (v < lo) v = hi;
    };
    bool changed = false;
    if (pressed[4]) loop(kGroove, kGAttack, 1, cfg.gauge), changed = true;
    if (pressed[2]) loop(0, 5, 1, cfg.random), changed = true;  // RANDOM
    if (pressed[6] && !down(7)) cfg.assist = !cfg.assist, changed = true;  // ASSIST（皿の自動演奏）
    if ((pressed[6] && held(7)) || (pressed[7] && held(6))) loop(0, 3, -1, cfg.effect), changed = true;
    if (pressed[5] && !down(7) && !down(6)) loop(cfg.hs_min, cfg.hs_max, -cfg.hs_margin, cfg.hi_speed), changed = true;
    if (pressed[7] && !down(5) && !down(6)) loop(cfg.hs_min, cfg.hs_max, cfg.hs_margin, cfg.hi_speed), changed = true;
    if ((pressed[5] && held(7)) || (pressed[7] && held(5))) {
        // HS-FIX。5 の MAIN BPM は OpenLR2 の拡張
        loop(0, 5, 1, cfg.hs_fix);
        changed = true;
    }
    if (pressed[3]) changed = true;  // BATTLE（未対応。LR2 と同じく音だけ鳴る）
    return changed;
}

// パネルの仮の絵。選曲スキンが読めるようになったら、スキンが描く（LR2: タイマー21、パネル1）
void draw_play_option_panel(SDL_Renderer* r, TextCache& text, const AppConfig& cfg, double open_ms) {
    static const char* gauges[] = {"GROOVE", "HARD", "DEATH", "EASY", "P-ATTACK", "G-ATTACK"};
    static const char* hs_fixes[] = {"OFF", "MAX BPM", "MIN BPM", "AVERAGE", "CONSTANT", "MAIN BPM"};
    static const char* effects[] = {"OFF", "HIDDEN", "SUDDEN", "HID+SUD"};
    // LR2 の WMIX と同じく、166ms で左から滑り込む
    float t = static_cast<float>(std::clamp(open_ms / 166.0, 0.0, 1.0));
    float w = 700, h = 330;
    float x = -w + (40 + w) * (1 - (1 - t) * (1 - t)), y = 150;
    SDL_FRect bg{x, y, w, h};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 18, 18, 30, 240);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 90, 90, 130, 255);
    SDL_RenderRect(r, &bg);
    SDL_Color white{235, 235, 240, 255}, dim{110, 110, 125, 255}, accent{255, 210, 90, 255};
    text.draw(x + 20, y + 14, 20, white, "PLAY OPTION");
    char hs[32];
    std::snprintf(hs, sizeof(hs), "%d.%02d", cfg.hi_speed / 100, cfg.hi_speed % 100);
    struct Key { const char* name; std::string value; bool ok; };
    // 上の段が黒鍵（2・4・6）、下の段が白鍵（1・3・5・7）
    static const char* randoms[] = {"OFF", "MIRROR", "RANDOM", "S-RANDOM", "SCATTER", "CONVERGE"};
    Key keys[8] = {{}, {"鍵盤数", "", false}, {"RANDOM", randoms[std::clamp(cfg.random, 0, 5)], true},
                   {"BATTLE", "OFF", false}, {"GAUGE", gauges[std::clamp(cfg.gauge, 0, 5)], true},
                   {"HI-SPEED −", hs, true}, {"ASSIST", cfg.assist ? "AUTO SCR." : "OFF", true}, {"HI-SPEED ＋", hs, true}};
    for (int k = 1; k <= 7; ++k) {
        bool black = k % 2 == 0;
        float kx = x + 30 + (k - 1) * 92, ky = y + (black ? 60 : 150);
        SDL_FRect kr{kx, ky, 84, 76};
        SDL_SetRenderDrawColor(r, black ? 40 : 70, black ? 40 : 70, black ? 55 : 90, 255);
        SDL_RenderFillRect(r, &kr);
        text.draw(kx + 42, ky + 6, 14, dim, std::to_string(k), 1);
        text.draw(kx + 42, ky + 26, 13, keys[k].ok ? white : dim, keys[k].name, 1, 84);
        text.draw(kx + 42, ky + 48, 13, keys[k].ok ? accent : dim, keys[k].value, 1, 84);
    }
    char line[128];
    std::snprintf(line, sizeof(line), "6+7  HIDDEN/SUDDEN : %s", effects[std::clamp(cfg.effect, 0, 3)]);
    text.draw(x + 30, y + 248, 16, white, line);
    std::snprintf(line, sizeof(line), "5+7  HS-FIX : %s", hs_fixes[std::clamp(cfg.hs_fix, 0, 5)]);
    text.draw(x + 30, y + 274, 16, white, line);
    text.draw(x + 30, y + 302, 13, dim, "START（キーボードは Q か Space）を押している間だけ開く");
}

}  // namespace

void run_select(App& app) {
    SDL_Renderer* r = app.renderer;
    TextCache text(r);
    app.set_view(1280, 720);

    // 手元にあるものですぐに一覧を出し、曲フォルダの読み直しと難易度表の更新は裏で行う
    auto lib = std::make_shared<Library>();
    lib->charts = app.db.all_charts();
    for (const std::string& u : app.cfg.tables) {
        DifficultyTable t;
        if (!load_cached_table(table_cache_dir(), u, t)) t.url = u, t.warning = "まだ取得していない";
        lib->tables.push_back(std::move(t));
    }
    for (const ScoreRecord& s : app.db.all_scores()) lib->scores[s.sha256] = s;
    std::vector<Entry> root = build_tree(*lib);

    std::mutex mu;
    std::shared_ptr<Library> pending;
    std::atomic<bool> busy{false};
    std::string status = "";
    std::thread worker;
    // scan_songs：曲フォルダも読み直すか（false なら難易度表だけ取りに行く）
    auto refresh = [&](bool scan_songs = true) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        worker = std::thread([&, scan_songs, dirs = app.cfg.song_dirs, urls = app.cfg.tables] {
            // 曲データベースは別の接続で書く（書き込みはこのスレッドだけ）
            SongDb wdb;
            ScanStats st;
            if (wdb.open(song_db_path()) && scan_songs)
                st = wdb.scan(dirs, static_cast<int>(std::max(2u, std::thread::hardware_concurrency())));
            auto next = std::make_shared<Library>();
            next->charts = wdb.all_charts();
            for (const ScoreRecord& s : wdb.all_scores()) next->scores[s.sha256] = s;
            for (const std::string& u : urls) next->tables.push_back(update_table(table_cache_dir(), u));
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d 譜面（追加 %d／変更 %d／削除 %d）%.1f 秒", st.found, st.added, st.updated,
                          st.removed, st.seconds);
            std::lock_guard<std::mutex> lock(mu);
            pending = next;
            status = buf;
            busy = false;
        });
    };
    // 起動時に曲フォルダと難易度表を読み直す（設定で「手動」なら F5 のときだけ）
    // 起動の画面で読み直し済みなら、ここでは読み直さない
    if (app.cfg.db_update == 0 && !app.updated_at_start) refresh();

    // 難易度の絞り込み（LR2: select.difficulty。0=ALL 1..5=BEGINNER..INSANE）。譜面の並ぶ一覧だけに効く
    int difficulty_filter = 0;
    // 鍵盤数の絞り込みとソートは、LR2 ではなく beatoraja の項目と並びに合わせる（BMjoy の拡張。LR2 のソートは実用にならないため）。
    // 鍵盤数：kModeNames の順。BMjoy の譜面は 7 鍵だけなので、ALL / 7KEY / SINGLE 以外では譜面が出ない
    int mode_filter = 0;
    // ソート：kSortNames の順。0 の OFF は並べ替えない（難易度表やフォルダの順のまま）
    int sort_mode = 0;
    auto lamp_of = [&](const ChartRecord* c) {
        auto it = lib->scores.find(c->sha256);
        return it != lib->scores.end() ? it->second.clear : 0;
    };
    struct Level {
        const std::vector<Entry>* full;  // 絞り込む前の一覧
        int cursor;
        std::string name;
        std::shared_ptr<std::vector<Entry>> view;  // 絞り込んだ一覧（絞り込んでいなければ空）
        const std::vector<Entry>* list;  // 見せている一覧
    };
    auto make_level = [&](const std::vector<Entry>* full, int cursor, const std::string& name) {
        Level lv{full, cursor, name, nullptr, full};
        if (difficulty_filter != 0 || !kModeShows7[mode_filter] || sort_mode != 0) {
            auto v = std::make_shared<std::vector<Entry>>();
            for (const Entry& e : *full) {
                if (e.kind != Entry::Kind::Folder && e.kind != Entry::Kind::Course) {
                    if (!kModeShows7[mode_filter]) continue;
                    if (difficulty_filter != 0 && (e.kind != Entry::Kind::Chart || e.chart->difficulty != difficulty_filter)) continue;
                }
                v->push_back(e);
            }
            // 譜面だけを並べ替える（フォルダは元の順のまま前、持っていない譜面は後ろ）。
            // 記録で並べるもの（CLEAR / SCORE / MISSCOUNT）は、記録の無い譜面を後ろに回す。同じならタイトル順
            if (sort_mode != 0)
                std::stable_sort(v->begin(), v->end(), [&](const Entry& a, const Entry& b) {
                    bool fa = a.kind == Entry::Kind::Folder || a.kind == Entry::Kind::Course,
                         fb = b.kind == Entry::Kind::Folder || b.kind == Entry::Kind::Course;
                    if (fa || fb) return fa && !fb;
                    const ChartRecord* ca = a.kind == Entry::Kind::Chart ? a.chart : nullptr;
                    const ChartRecord* cb = b.kind == Entry::Kind::Chart ? b.chart : nullptr;
                    if (!ca || !cb) return ca != nullptr && cb == nullptr;
                    auto lower = [](std::string s) {
                        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return s;
                    };
                    auto by_title = [&] {
                        std::string ta = lower(ca->title), tb = lower(cb->title);
                        return ta != tb ? ta < tb : ca->difficulty < cb->difficulty;
                    };
                    const ScoreRecord* sa = nullptr;
                    const ScoreRecord* sb = nullptr;
                    if (auto it = lib->scores.find(ca->sha256); it != lib->scores.end() && it->second.playcount > 0) sa = &it->second;
                    if (auto it = lib->scores.find(cb->sha256); it != lib->scores.end() && it->second.playcount > 0) sb = &it->second;
                    auto by_record = [&](auto key) {
                        if (!sa || !sb) return sa != nullptr && sb == nullptr ? true : (!sa && !sb ? by_title() : false);
                        auto ka = key(*sa, *ca), kb = key(*sb, *cb);
                        return ka != kb ? ka < kb : by_title();
                    };
                    switch (sort_mode) {
                    case 1: return by_title();
                    case 2: {
                        std::string xa = lower(ca->artist), xb = lower(cb->artist);
                        return xa != xb ? xa < xb : by_title();
                    }
                    case 3: return ca->bpm_max != cb->bpm_max ? ca->bpm_max < cb->bpm_max : by_title();
                    case 4: return ca->length_ms != cb->length_ms ? ca->length_ms < cb->length_ms : by_title();
                    case 5:
                        if (ca->playlevel != cb->playlevel) return ca->playlevel < cb->playlevel;
                        return ca->difficulty != cb->difficulty ? ca->difficulty < cb->difficulty : by_title();
                    case 6: return by_record([](const ScoreRecord& s, const ChartRecord&) { return static_cast<double>(s.clear); });
                    case 7:
                        return by_record([](const ScoreRecord& s, const ChartRecord& c) {
                            return c.notes > 0 ? static_cast<double>(s.exscore) / (c.notes * 2) : 0.0;
                        });
                    default: return by_record([](const ScoreRecord& s, const ChartRecord&) { return static_cast<double>(s.bp); });
                    }
                });
            lv.view = v;
            lv.list = v.get();
        }
        return lv;
    };
    std::vector<Level> stack{make_level(&root, 0, "BMjoy")};
    // フォルダ → その中の譜面（一覧を差し替えたら作り直す）
    std::unordered_map<std::string, std::vector<const ChartRecord*>> folder_charts;
    app.mixer.play_system(kSysSelect, true);  // 選曲BGM（ループ）
    bool autoplay = false;
    InputMapper input(app.keycfg);
    input.set_min_hold_ms(app.cfg.input_interval);
    std::vector<std::string> joy_names;
    bool joy_reported = false;
    // パネル（LR2: 1=プレイオプション 2=システムオプション …。-1 は無し）。START か Space を押している間は 1
    int panel = -1;
    bool space_held = false, force_panel = false, start_was_held = false;
    uint64_t panel_open_ns = 0;
    bool up_held = false, down_held = false;
    int prev_hold_dir = 0;
    bool quit = false;

    // 選曲スキン（docs/lr2_select_spec.md）。読めなければ BMjoy の仮の画面
    OptionNames opt_names;
    opt_names.load(app.lr2_root);
    SelectContext sctx;
    sctx.cfg = &app.cfg;
    sctx.names = &opt_names;
    {
        std::array<bool, 1000> ops{};
        for (int i = 0; i < 900; ++i) ops[i] = sctx.option(i);
        app.load_select_skin(ops);
    }
    Lr2Skin* sk = app.select_skin.get();
    sctx.skin = sk;
    // スキンセレクト（WMIX のパネル2・LR2 標準のスキンセレクト画面）
    SkinCatalog catalog;
    catalog.scan(app.lr2_root);
    sctx.catalog = &catalog;
    sctx.player = app.db.player_stats();
    std::unordered_map<std::string, bool> folder_has_text;  // 曲のフォルダに .txt があるか（op 175）
    auto set_select_view = [&] {
        if (sk) app.set_view(sk->width, sk->height);
        else app.set_view(1280, 720);
    };
    set_select_view();
    const uint64_t scene_start = SDL_GetTicksNS();
    auto scene_ms = [&] { return (SDL_GetTicksNS() - scene_start) / 1e6; };
    sctx.timers.set(0, 0);
    // 曲リストのスクロール（仕様 3.1・3.5。1曲=1000。カーソルの曲は lv.cursor が持ち、見た目だけを補間する）
    int64_t list_top = 0, old_bar = 0, now_bar = 0;
    double move_start = 0, move_end = 0;
    float mouse_x = -1, mouse_y = -1;
    double mouse_moved_ms = -1e9;
    const SkinObject* dragging = nullptr;  // つかんでいるスライダー（種別1：曲リスト、7：カスタマイズ項目）
    // 選曲スキン（やそのカスタマイズ）を替えたら、読み直して描き直す
    auto reload_select_skin = [&] {
        std::array<bool, 1000> ops{};
        for (int i = 0; i < 900; ++i) ops[i] = sctx.option(i);
        app.load_select_skin(ops);
        sk = app.select_skin.get();
        sctx.skin = sk;
        dragging = nullptr;
        set_select_view();
    };
    // テキストビューア（readme・ヘルプ。仕様 5.4）
    std::vector<std::string> readme_lines;
    bool readme_open = false;
    float readme_x = 0, readme_y = 0;
    auto readme_line_h = [&] { return sk && sk->readme[0].src.op[0] > 0 ? static_cast<float>(sk->readme[0].src.op[0]) : 20.0f; };
    auto open_readme = [&](const std::vector<std::string>& paths) {
        std::vector<std::string> lines;
        for (size_t i = 0; i < paths.size(); ++i) {
            std::ifstream f(paths[i], std::ios::binary);
            if (!f) continue;
            std::string text = to_utf8_auto(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
            // 複数のファイルを続けて見せるときは、それぞれの頭に「n/総数 ファイル名」を入れる
            if (paths.size() > 1)
                lines.push_back(std::to_string(i + 1) + "/" + std::to_string(paths.size()) + " " + fs::path(paths[i]).filename().string());
            std::string line;
            std::istringstream in(text);
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string out;
                for (char ch : line) out += ch == '\t' ? std::string("    ") : std::string(1, ch);
                lines.push_back(out);
            }
        }
        if (lines.empty()) return;
        readme_lines = std::move(lines);
        readme_open = true;
        readme_x = readme_y = 0;
        sctx.timers.reset(16);
        sctx.timers.set(15, scene_ms());
        app.mixer.play_system(kSysOptionChange);
    };
    auto close_readme = [&] {
        if (!readme_open) return;
        readme_open = false;
        sctx.timers.reset(15);
        sctx.timers.set(16, scene_ms());
    };
    // バナー（#RELOADBANNER のスキンだけ。仕様 2.17）。カーソルの曲が変わるたびに裏で読み、読めたら 102 番の画像にする
    struct BannerLoad {
        std::mutex mu;
        std::string path;   // 読み込み中・読み込んだもの
        SkinImage image;
        bool ready = false;
    };
    auto banner = std::make_shared<BannerLoad>();
    std::thread banner_thread;
    std::string banner_want;  // 今のカーソルの曲のバナー
    // 曲のプレビュー（LR2: Songselect.cpp:2710）。カーソルを 500ms 置いた曲を裏で読み、曲の頭からオートで鳴らす。
    // 鳴らしている間は選曲 BGM を 1 秒かけて小さくし、カーソルが別の曲に移ったら止めて戻す
    struct Preview {
        Chart chart;
        SoundBank bank;
        std::string path;
    };
    std::shared_ptr<Preview> preview;            // 鳴らしているもの
    std::future<std::shared_ptr<Preview>> preview_load;
    std::vector<std::future<std::shared_ptr<Preview>>> stale_loads;  // 要らなくなった読み込み（終わるのを待たずに手放す）
    std::string preview_path;                    // 読んでいる・鳴らしている譜面（空なら何もしていない）
    auto stop_preview = [&](bool restore_bgm) {
        if (preview) {
            app.mixer.stop();
            app.mixer.wait_callback();
            preview.reset();
        }
        preview_path.clear();
        if (restore_bgm) app.mixer.fade_system_loop(1.0f, 1000);
    };
    SDL_Texture* banner_tex = nullptr;
    auto banner_path_of = [](const ChartRecord& c) -> std::string {
        if (c.banner.empty()) return {};
        fs::path p = fs::path(c.folder) / c.banner;
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
        // 拡張子だけ違うことがよくある
        for (const char* ext : {".png", ".jpg", ".bmp", ".PNG", ".JPG", ".BMP"}) {
            fs::path q = p;
            q.replace_extension(ext);
            if (fs::exists(q, ec)) return q.string();
        }
        return {};
    };

    auto current = [&]() -> const Entry* {
        const Level& lv = stack.back();
        if (lv.list->empty()) return nullptr;
        return &(*lv.list)[std::clamp(lv.cursor, 0, static_cast<int>(lv.list->size()) - 1)];
    };
    // 一覧が変わったとき：補間せずにカーソルの位置へ置く（仕様 3.7）
    auto snap = [&] {
        list_top = old_bar = now_bar = static_cast<int64_t>(stack.back().cursor) * 1000;
        move_start = move_end = scene_ms();
        sctx.timers.set(11, scene_ms());
    };
    // duration：補間にかける時間（LR2: speedfirst 300ms / speednext 70ms / ホイール 200ms）
    auto move = [&](int d, double duration = 300) {
        Level& lv = stack.back();
        int n = static_cast<int>(lv.list->size());
        if (n) lv.cursor = ((lv.cursor + d) % n + n) % n;
        double t = scene_ms();
        old_bar = list_top;
        now_bar += static_cast<int64_t>(d) * 1000;
        move_start = t;
        move_end = t + duration;
        sctx.timers.set(11, t);
        app.mixer.play_system(kSysScratch);
    };
    // パネルを開く・閉じる（仕様 5.1）。p = -1 で閉じる
    auto set_panel = [&](int p, bool sound = true) {
        double t = scene_ms();
        if (panel > 0) sctx.timers.reset(20 + panel), sctx.timers.set(30 + panel, t);
        if (p > 0) sctx.timers.reset(30 + p), sctx.timers.set(20 + p, t), panel_open_ns = SDL_GetTicksNS();
        if (sound && p != panel) app.mixer.play_system(p > 0 ? kSysPanelOpen : kSysPanelClose);
        panel = p;
    };
    // 一覧を差し替えたあと、いま見ているフォルダの位置を名前でたどり直す
    auto rebuild = [&] {
        std::vector<std::string> path;
        std::vector<int> cursors;
        for (size_t i = 1; i < stack.size(); ++i) path.push_back(stack[i].name);
        for (const Level& lv : stack) cursors.push_back(lv.cursor);
        root = build_tree(*lib);
        stack = {make_level(&root, cursors[0], "BMjoy")};
        for (size_t i = 0; i < path.size(); ++i) {
            const std::vector<Entry>& list = *stack.back().list;
            auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.label == path[i]; });
            if (it == list.end()) break;
            stack.push_back(make_level(&it->children, cursors[i + 1], path[i]));
        }
        int n = static_cast<int>(stack.back().list->size());
        stack.back().cursor = n ? std::clamp(stack.back().cursor, 0, n - 1) : 0;
        snap();
    };
    auto decide = [&] {
        const Entry* e = current();
        if (!e) return;
        if (e->kind == Entry::Kind::Folder) {
            stack.push_back(make_level(&e->children, 0, e->label));
            snap();
            app.mixer.play_system(kSysFolderOpen);
        } else if (e->kind == Entry::Kind::Chart || (e->kind == Entry::Kind::Course && e->course->playable())) {
            // 遊べないコース（譜面が欠けている）は、決定しても何も起きない（LR2 と同じ）
            PlayOptions po;
            po.autoplay = autoplay;
            if (const char* cap = std::getenv("BMJ_PLAY_CAPTURE")) po.capture_dir = cap;  // 開発用
            std::string path = e->chart ? e->chart->path : "";
            std::shared_ptr<Course> course = e->course;
            stop_preview(false);
            if (preview_load.valid()) stale_loads.push_back(std::move(preview_load));
            app.mixer.fade_system_loop(1.0f, 0);
            app.mixer.stop_system(kSysSelect);
            // 決定画面（スキンが無ければ出さない）
            {
                SelectContext dc = sctx;
                dc.chart = e->chart;
                dc.course = course.get();
                dc.type = course ? 8 : 0;
                auto it = lib->scores.find(course ? course->hash : e->chart->sha256);
                dc.score = it != lib->scores.end() ? &it->second : nullptr;
                DecideResult dr = run_decide(app, dc);
                if (dr != DecideResult::Play) {
                    if (dr == DecideResult::Quit) quit = true;
                    app.mixer.play_system(kSysSelect, true);
                    set_select_view();
                    return;
                }
                po.decide_sound = !app.decide_skin;
            }
            PlayExit ex = course ? run_course(app, *course, po) : run_play(app, path, po);
            app.save_config();  // 演奏中に変えたハイスピードやカバーを残す
            app.mixer.play_system(kSysSelect, true);
            set_select_view();
            input = InputMapper(app.keycfg), input.set_min_hold_ms(app.cfg.input_interval);
            // スコアとプレイヤーの累計を読み直す
            for (const ScoreRecord& s : app.db.all_scores()) lib->scores[s.sha256] = s;
            sctx.player = app.db.player_stats();
            if (ex == PlayExit::Quit) quit = true;
        } else if (e->kind == Entry::Kind::Missing) {
            std::lock_guard<std::mutex> lock(mu);
            status = "この譜面は持っていない：" + e->missing.title;
        }
    };
    // 難易度の切り替え（LR2: Songselect.cpp の SELECT / →。仕様 4.7）。同じフォルダの、今より上の難易度の譜面へ移る。
    // 上が無ければ ALL に戻す
    auto change_difficulty = [&](int to = -1) {
        const Entry* e = current();
        const ChartRecord* target = e && e->kind == Entry::Kind::Chart ? e->chart : nullptr;
        if (to < 0) {
            if (!target) return;
            to = 0;
            int best = 99;
            for (const ChartRecord& c : lib->charts)
                if (c.folder == target->folder && c.difficulty > target->difficulty && c.difficulty <= 5 && c.difficulty < best)
                    best = c.difficulty;
            if (best != 99) {
                to = best;
                for (const ChartRecord& c : lib->charts)
                    if (c.folder == target->folder && c.difficulty == best) {
                        target = &c;
                        break;
                    }
            }
        }
        else if (to > 0 && target) {
            // 難易度を直接選んだとき（スキンのボタン 91..96）は、同じフォルダのその難易度の譜面へ
            for (const ChartRecord& c : lib->charts)
                if (c.folder == target->folder && c.difficulty == to) {
                    target = &c;
                    break;
                }
        }
        difficulty_filter = to;
        Level& lv = stack.back();
        lv = make_level(lv.full, 0, lv.name);
        // 移った先の譜面にカーソルを合わせる
        for (size_t i = 0; i < lv.list->size(); ++i)
            if (target && (*lv.list)[i].kind == Entry::Kind::Chart && (*lv.list)[i].chart == target) lv.cursor = static_cast<int>(i);
        snap();
        app.mixer.play_system(app.mixer.has_system(kSysDifficulty) ? kSysDifficulty : kSysFolderOpen);
    };
    // 絞り込みやソートを変えたら、いまの一覧を作り直して、同じ要素にカーソルを合わせる
    auto reapply_view = [&] {
        const Entry* e = current();
        std::string label = e ? e->label : "";
        const ChartRecord* chart = e && e->kind == Entry::Kind::Chart ? e->chart : nullptr;
        Level& lv = stack.back();
        lv = make_level(lv.full, 0, lv.name);
        for (size_t i = 0; i < lv.list->size(); ++i) {
            const Entry& x = (*lv.list)[i];
            if ((chart && x.kind == Entry::Kind::Chart && x.chart == chart) || (!chart && x.label == label)) {
                lv.cursor = static_cast<int>(i);
                break;
            }
        }
        snap();
        app.mixer.play_system(kSysOptionChange);
    };
    auto back = [&] {
        if (stack.size() > 1) {
            stack.pop_back();
            snap();
            app.mixer.play_system(kSysFolderClose);
        }
    };

    // 検索（仕様 5.5）。スキンの文字列 30 の欄をクリックして入力し、Enter で検索する
    bool editing = false;
    std::string edit_buf;
    std::list<std::vector<Entry>> search_results;  // 検索結果の一覧（スタックから指すので消さずに持つ）
    auto run_search = [&](const std::string& q) {
        if (q.empty()) return;
        auto lower = [](std::string x) {
            for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return x;
        };
        // 数字だけなら LEVEL、「a-b」ならその範囲の LEVEL、50 以上なら BPM で探す。それ以外は文字で探す（LR2 と同じ）
        int lo = -1, hi = -1, bpm = -1;
        size_t dash = q.find('-');
        auto digits = [](const std::string& x) { return !x.empty() && std::all_of(x.begin(), x.end(), ::isdigit); };
        if (digits(q)) {
            int v = std::atoi(q.c_str());
            if (v >= 50) bpm = v;
            else lo = hi = v;
        } else if (dash != std::string::npos && digits(q.substr(0, dash)) && digits(q.substr(dash + 1))) {
            lo = std::atoi(q.substr(0, dash).c_str()), hi = std::atoi(q.substr(dash + 1).c_str());
        }
        std::string lq = lower(q);
        std::vector<Entry> hits;
        for (const ChartRecord& c : lib->charts) {
            bool ok;
            if (bpm >= 0) ok = static_cast<int>(c.bpm_max) == bpm || static_cast<int>(c.bpm_min) == bpm;
            else if (lo >= 0) ok = lo <= c.playlevel && c.playlevel <= hi;
            else
                ok = lower(c.title).find(lq) != std::string::npos || lower(c.subtitle).find(lq) != std::string::npos ||
                     lower(c.artist).find(lq) != std::string::npos || lower(c.subartist).find(lq) != std::string::npos ||
                     lower(c.genre).find(lq) != std::string::npos;
            if (!ok) continue;
            Entry e;
            e.kind = Entry::Kind::Chart;
            e.chart = &c;
            e.label = c.title + (c.subtitle.empty() ? "" : " " + c.subtitle);
            e.sub = c.artist;
            hits.push_back(std::move(e));
            if (app.cfg.search_max > 0 && static_cast<int>(hits.size()) >= app.cfg.search_max) break;  // LR2: searchmax
        }
        std::string name = q + (hits.size() == 1 ? " (1 HIT)" : " (" + std::to_string(hits.size()) + " HITS)");
        std::printf("検索: %s\n", name.c_str());
        if (hits.empty()) return;
        search_results.push_back(std::move(hits));
        stack.push_back(make_level(&search_results.back(), 0, name));
        snap();
        app.mixer.play_system(kSysFolderOpen);
    };
    auto finish_edit = [&](bool search) {
        editing = false;
        SDL_StopTextInput(app.window);
        sctx.timers.set(4, scene_ms());
        if (search) run_search(edit_buf);
        edit_buf.clear();
    };
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                if (SDL_Joystick* j = SDL_OpenJoystick(e.jdevice.which)) {
                    const char* name = SDL_GetJoystickName(j);
                    std::printf("コントローラー接続: %s（ボタン %d、軸 %d、ハット %d）\n", name ? name : "?",
                                SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickHats(j));
                    joy_names.push_back(name ? name : "?");
                }
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) std::printf("ウィンドウにフォーカスが来た\n");
            if (std::getenv("BMJ_DEBUG_INPUT") && e.type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && e.type <= SDL_EVENT_JOYSTICK_UPDATE_COMPLETE) {
                static int logged = 0;
                if (logged++ < 30) std::printf("選曲画面: ジョイスティックのイベント %u を受け取った\n", e.type);
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) std::printf("ウィンドウのフォーカスが外れた\n");
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y, mouse_moved_ms = scene_ms();
            }
            if (e.type == SDL_EVENT_MOUSE_MOTION && dragging && dragging->src.op[2] == 7) {
                // カスタマイズ項目のスクロール
                const SkinSrc& src = dragging->src;
                SkinDraw d = eval_dst(dragging->dst, sctx.timers.lapse(dragging->dst.timer, scene_ms()));
                float len = static_cast<float>(std::max(1, src.op[1]));
                float r = src.op[0] == 0 ? (d.y + d.h / 2 - mouse_y) / len : src.op[0] == 1 ? (mouse_x - d.x - d.w / 2) / len
                          : src.op[0] == 2 ? (mouse_y - d.y - d.h / 2) / len : (d.x + d.w / 2 - mouse_x) / len;
                catalog.set_scroll(static_cast<int>(std::lround(std::clamp(r, 0.0f, 1.0f) * catalog.scroll_max(app.cfg))), app.cfg);
            } else if (e.type == SDL_EVENT_MOUSE_MOTION && dragging && !stack.back().list->empty()) {
                // スライダーの位置 → 曲リストの位置（仕様 6.6）。つかんでいる間はそのまま動かす
                const SkinSrc& src = dragging->src;
                SkinDraw d = eval_dst(dragging->dst, sctx.timers.lapse(dragging->dst.timer, scene_ms()));
                float len = static_cast<float>(std::max(1, src.op[1]));
                float r = 0;
                switch (src.op[0]) {
                case 0: r = (d.y + d.h / 2 - mouse_y) / len; break;
                case 1: r = (mouse_x - d.x - d.w / 2) / len; break;
                case 2: r = (mouse_y - d.y - d.h / 2) / len; break;
                default: r = (d.x + d.w / 2 - mouse_x) / len; break;
                }
                r = std::clamp(r, 0.0f, 1.0f);
                Level& lv = stack.back();
                int n = static_cast<int>(lv.list->size());
                int64_t v = static_cast<int64_t>(r * (n * 1000 - 1));
                list_top = old_bar = now_bar = v;
                move_start = move_end = scene_ms();
                int cur = static_cast<int>((v + 500) / 1000) % n;
                if (cur != lv.cursor) lv.cursor = cur, sctx.timers.set(11, scene_ms()), app.mixer.play_system(kSysScratch);
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && dragging && dragging->src.op[2] == 7) {
                dragging = nullptr;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && dragging) {
                // 離したら最寄りの曲にスナップする（100ms）
                dragging = nullptr;
                old_bar = list_top;
                now_bar = static_cast<int64_t>(stack.back().cursor) * 1000;
                if (now_bar - old_bar > 500) now_bar -= static_cast<int64_t>(stack.back().list->size()) * 1000;
                move_start = scene_ms();
                move_end = move_start + 100;
            }
            if (editing) {
                // 検索欄に入力している間は、文字の入力だけ
                if (e.type == SDL_EVENT_TEXT_INPUT) edit_buf += e.text.text;
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_BACKSPACE && !edit_buf.empty()) {
                        size_t i = edit_buf.size() - 1;
                        while (i > 0 && (static_cast<unsigned char>(edit_buf[i]) & 0xC0) == 0x80) --i;
                        edit_buf.erase(i);
                    } else if (e.key.key == SDLK_V && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
                        if (char* c = SDL_GetClipboardText()) edit_buf += c, SDL_free(c);
                    } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                        finish_edit(true);
                    } else if (e.key.key == SDLK_ESCAPE) {
                        finish_edit(false);
                    }
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) finish_edit(false);
                continue;
            }
            if (readme_open) {
                // テキストを見ている間は、スクロールと閉じる操作だけ
                if (e.type == SDL_EVENT_MOUSE_WHEEL) readme_y += e.wheel.y * readme_line_h();
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) close_readme();
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                    (e.key.key == SDLK_RETURN || e.key.key == SDLK_ESCAPE || e.key.key == SDLK_BACKSPACE))
                    close_readme();
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_UP) up_held = true;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_DOWN) down_held = true;
                if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_UP) up_held = false;
                if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_DOWN) down_held = false;
                input.handle(e);
                continue;
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL && sk) {
                // ホイール：上で前の曲へ。前の移動の終わりは待たない（仕様 3.5）
                int w = static_cast<int>(std::lround(e.wheel.y));
                if (w) move(-w, 200);
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT && sk) {
                // 右クリック：パネルが開いていれば閉じる、無ければ1階層戻る（仕様 4.4）
                if (panel > 0) set_panel(-1);
                else back();
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && sk) {
                float mx = e.button.x, my = e.button.y;
                double t = scene_ms();
                bool clicked = false;
                // スキンのボタン（仕様 6.7）。右半分で +1、左半分で -1（op4=1 ならどこでも +1）
                for (const SkinObject& o : sk->buttons) {
                    if (o.src.op[1] != 1 || o.dst.empty() || !sctx.timers.on(o.dst.timer)) continue;
                    int bp = o.src.op[2];
                    if (bp != 0 && bp != panel) continue;
                    if (!sctx.option(o.dst.op[0]) || !sctx.option(o.dst.op[1]) || !sctx.option(o.dst.op[2])) continue;
                    SkinDraw d = eval_dst(o.dst, sctx.timers.lapse(o.dst.timer, t));
                    if (!d.visible) continue;
                    float x0 = std::min(d.x, d.x + d.w), x1 = std::max(d.x, d.x + d.w);
                    float y0 = std::min(d.y, d.y + d.h), y1 = std::max(d.y, d.y + d.h);
                    if (mx < x0 || mx >= x1 || my < y0 || my >= y1) continue;
                    int dir = o.src.op[3] == 1 ? 1 : o.src.op[3] == 2 ? -1 : (mx >= (x0 + x1) / 2 ? 1 : -1);
                    clicked = true;
                    int ty = o.src.op[0];
                    auto loop = [&](int& v, int lo, int hi) {
                        v += dir;
                        if (v > hi) v = lo;
                        else if (v < lo) v = hi;
                        app.mixer.play_system(kSysOptionChange);
                    };
                    if (ty >= 1 && ty <= 8) set_panel(panel == ty ? -1 : ty);
                    else if (ty == 13) {
                        if (run_key_wizard(r, text, app.keycfg, keyconfig_path())) app.mixer.play_system(kSysDecide);
                        input = InputMapper(app.keycfg), input.set_min_hold_ms(app.cfg.input_interval);
                        set_select_view();
                    } else if (ty == 15 || ty == 16) {
                        bool was = autoplay;
                        autoplay = ty == 16;
                        decide();
                        autoplay = was;
                    } else if (ty == 40) loop(app.cfg.gauge, kGroove, kGAttack);
                    else if (ty == 46) app.cfg.lane_cover = !app.cfg.lane_cover, app.mixer.play_system(kSysOptionChange);
                    else if (ty == 42) loop(app.cfg.random, 0, 5);
                    else if (ty == 17) {
                        // 曲のフォルダの .txt を全部見せる
                        const Entry* cur = current();
                        if (cur && cur->kind == Entry::Kind::Chart) {
                            std::vector<std::string> txt;
                            std::error_code ec;
                            for (const auto& de : fs::directory_iterator(cur->chart->folder, ec)) {
                                std::string ext = de.path().extension().string();
                                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                                if (ext == ".txt") txt.push_back(de.path().string());
                            }
                            std::sort(txt.begin(), txt.end());
                            open_readme(txt);
                        }
                    } else if (ty >= 200 && ty <= 209) {
                        // #HELPFILE の n 番
                        size_t n = static_cast<size_t>(ty - 200);
                        if (n < sk->helpfiles.size()) {
                            std::string p = resolve_lr2_path(app.lr2_root, sk->helpfiles[n], false);
                            if (!p.empty()) open_readme({p});
                        }
                    }
                    else if (ty >= 170 && ty <= 188) {
                        catalog.select_type(ty - 170, app.cfg);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 190 || (ty >= 220 && ty <= 229)) {
                        // スキンを替える／カスタマイズ項目を替える。選曲スキンなら、その場で読み直す
                        bool changed = ty == 190 ? catalog.step_skin(app.cfg, dir) : catalog.step_custom(app.cfg, ty - 220, dir);
                        if (changed) {
                            // サウンドセットはその場で読み直し、選曲の BGM も新しい音で鳴らし直す
                            if (catalog.selected_type() == 10 && ty == 190) {
                                app.load_sound_set();
                                app.mixer.play_system(kSysSelect, true);
                            }
                            app.mixer.play_system(kSysOptionChange);
                            app.save_config();
                            if (catalog.selected_type() == 5) {
                                reload_select_skin();
                                break;
                            }
                        }
                    } else if (ty == 14) {
                        // スキンセレクト画面
                        int old_panel = panel;
                        set_panel(-1, false);
                        bool select_changed = run_skin_select(app, catalog);
                        app.save_config();
                        if (select_changed) reload_select_skin();
                        else set_select_view();
                        (void)old_panel;
                        break;
                    }
                    else if (ty == 10) change_difficulty((difficulty_filter + 1) % 6);
                    else if (ty == 11 || ty == 12) {
                        // 鍵盤数の絞り込み・ソート（左右どちらでも +1。ソートは LR2 と同じく 0..4）
                        int& v = ty == 11 ? mode_filter : sort_mode;
                        v = (v + 1) % (ty == 11 ? kModeCount : kSortCount);
                        reapply_view();
                    }
                    else if (ty >= 91 && ty <= 96) change_difficulty(ty - 91);
                    else if (ty == 44) app.cfg.assist = !app.cfg.assist, app.mixer.play_system(kSysOptionChange);
                    else if (ty == 50) loop(app.cfg.effect, 0, 3);
                    else if (ty == 55) {
                        loop(app.cfg.hs_fix, 0, 5);
                    } else if (ty == 57) {
                        app.cfg.hi_speed = std::clamp(app.cfg.hi_speed + dir * app.cfg.hs_margin, app.cfg.hs_min, app.cfg.hs_max);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 74) {
                        app.cfg.judge_timing = std::clamp(app.cfg.judge_timing + dir, -199, 199);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 80) {
                        bool fsc = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN;
                        SDL_SetWindowFullscreen(app.window, !fsc);
                    }
                    break;
                }
                // 検索欄（文字列 30 で edit=1 のもの）をクリックしたら入力を始める
                if (!clicked)
                    for (const SkinText& tx : sk->texts) {
                        if (tx.src.st != 30 || tx.src.op[0] != 1 || tx.dst.empty()) continue;
                        int tp = tx.src.op[1];
                        if (tp != 0 && tp != panel) continue;
                        SkinDraw d = eval_dst(tx.dst, sctx.timers.lapse(tx.dst.timer, t));
                        if (!d.visible) continue;
                        float x0 = tx.src.align == 1 ? d.x - d.w / 2 : tx.src.align == 2 ? d.x - d.w : d.x;
                        if (mx >= x0 && mx < x0 + d.w && my >= d.y && my < d.y + d.h) {
                            editing = true;
                            edit_buf.clear();
                            SDL_StartTextInput(app.window);
                            clicked = true;
                            break;
                        }
                    }
                // 曲リストのスライダーをつかむ
                if (!clicked)
                    for (const SkinObject& o : sk->sliders) {
                        if ((o.src.op[2] != 1 && o.src.op[2] != 7) || o.src.op[3] == 1 || o.dst.empty() || !sctx.timers.on(o.dst.timer)) continue;
                        SkinDraw d = eval_dst(o.dst, sctx.timers.lapse(o.dst.timer, t));
                        if (!d.visible) continue;
                        float len = static_cast<float>(o.src.op[1]);
                        SDL_FRect rc{d.x, d.y, d.w, d.h};
                        if (o.src.op[0] == 0) rc.y -= len, rc.h += len;
                        else if (o.src.op[0] == 1) rc.w += len;
                        else if (o.src.op[0] == 2) rc.h += len;
                        else rc.x -= len, rc.w += len;
                        if (mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h) {
                            dragging = &o;
                            clicked = true;
                            break;
                        }
                    }
                // 曲バー（仕様 3.6）：選んでいる行を押したら決定、ほかの行はその行がカーソルに来るまで送る
                if (!clicked && !stack.back().list->empty()) {
                    const Lr2Skin::Bar& b = sk->bar;
                    for (int i = 1; i < 30; ++i) {
                        if (b.body_off[i].empty()) continue;
                        bool on = i == b.center && !b.body_on[i].empty();
                        SkinDraw d = eval_dst(on ? b.body_on[i] : b.body_off[i], t);
                        if (!d.visible || mx < d.x || mx >= d.x + d.w || my < d.y || my >= d.y + d.h) continue;
                        if (i == b.center) decide();
                        else move(i - b.center, 200);
                        break;
                    }
                }
            }
            if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_UP) up_held = false;
            if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_DOWN) down_held = false;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_UP: up_held = true; break;
                case SDLK_DOWN: down_held = true; break;
                case SDLK_PAGEUP: move(-10); break;
                case SDLK_PAGEDOWN: move(10); break;
                case SDLK_RETURN: if (!e.key.repeat) decide(); break;
                case SDLK_RIGHT: {
                    // → はフォルダなら開く、譜面なら難易度の切り替え（LR2 と同じ）
                    if (e.key.repeat) break;
                    const Entry* cur = current();
                    if (cur && cur->kind == Entry::Kind::Chart) change_difficulty();
                    else decide();
                    break;
                }
                case SDLK_LEFT:
                case SDLK_BACKSPACE: back(); break;
                case SDLK_ESCAPE:
                    if (e.key.repeat) break;
                    if (stack.size() > 1) back();
                    else quit = true;
                    break;
                case SDLK_F5: refresh(); break;
                case SDLK_F1:
                    if (e.key.repeat) break;
                    // キーコンフィグ（押した入力を順に1〜7鍵・皿に割り当てる）
                    if (run_key_wizard(r, text, app.keycfg, keyconfig_path())) app.mixer.play_system(kSysDecide);
                    input = InputMapper(app.keycfg), input.set_min_hold_ms(app.cfg.input_interval);
                    break;
                case SDLK_F2:
                    autoplay = !autoplay;
                    app.mixer.play_system(kSysOptionChange);
                    break;
                case SDLK_SPACE: space_held = true; break;
                case SDLK_F6:
                    // レーンカバー（SUDDEN+）を使うか
                    app.cfg.lane_cover = !app.cfg.lane_cover;
                    app.mixer.play_system(kSysOptionChange);
                    break;
                case SDLK_F4: {
                    bool fsc = SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(app.window, !fsc);
                    break;
                }
                default: break;
                }
            }
            if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_SPACE) space_held = false;
            input.handle(e);
        }
        // コントローラ：白鍵で決定、黒鍵で戻る、皿で上下（LR2の選曲画面と同じ）
        uint64_t now = SDL_GetTicksNS();
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        input.collect(now, lanes, buttons);
        double t_ms = scene_ms();
        // 入力の受付開始（#STARTINPUT。LR2 のタイマー1）
        if (!sctx.timers.on(1) && (!sk || t_ms > sk->startinput)) sctx.timers.set(1, t_ms);
        // 鍵盤の光（タイマー 100+鍵 / 120+鍵）
        for (const LaneInput& li : lanes) {
            if (li.down) sctx.timers.set(100 + li.lane, t_ms), sctx.timers.reset(120 + li.lane);
            else sctx.timers.reset(100 + li.lane), sctx.timers.set(120 + li.lane, t_ms);
        }
        // プレイオプションのパネル（仕様 5.2）。START か Space を押している間だけ開く。マウスで開いていたら START で閉じる
        bool start_held = input.button_held(kStart) || space_held || force_panel;
        if (start_held && !start_was_held) set_panel(panel == 1 ? -1 : 1, panel != 1);
        else if (!start_held && start_was_held && panel == 1) set_panel(-1);
        start_was_held = start_held;
        if (panel == 1) {
            // 開いている間は、鍵盤はオプションの変更に使う
            if (play_option_keys(app.cfg, input, buttons)) app.mixer.play_system(kSysOptionChange);
            buttons.clear();
        }
        bool accept = sctx.timers.on(1) && !readme_open && !editing;
        if (editing) buttons.clear();
        if (readme_open) {
            // 鍵盤のどれかで閉じる。↑で下へ 0.6px/ms、↓で上へ 1.2px/ms（LR2 と同じく下へ送る方が速い）
            for (const ButtonInput& b : buttons)
                if (b.down && b.button >= kKey1 && b.button <= kKey7) close_readme();
            static double last = t_ms;
            double dt = std::min(100.0, t_ms - last);
            if (up_held) readme_y += static_cast<float>(0.6 * dt);
            if (down_held) readme_y -= static_cast<float>(1.2 * dt);
            last = t_ms;
            buttons.clear();
        }
        if (sk) {
            float total = readme_line_h() * readme_lines.size() + (sk->readme[0].dst.empty() ? 0 : sk->readme[0].dst.keys.front().y);
            float lo = std::min(0.0f, sk->height - total);
            readme_y = std::clamp(readme_y, lo, 0.0f);
        }
        // 鍵盤（LR2: 仕様 4.2）：1・3・5・7 で決定、2 で戻る、6 でオートで開始、SELECT で難易度の切り替え
        for (const ButtonInput& b : buttons) {
            if (!b.down || !accept) continue;
            if (b.button == kKey1 || b.button == kKey1 + 2 || b.button == kKey1 + 4 || b.button == kKey1 + 6) decide();
            else if (b.button == kKey1 + 1) back();
            else if (b.button == kKey1 + 5) {
                const Entry* e = current();
                if (e && e->kind == Entry::Kind::Chart) {
                    bool was = autoplay;
                    autoplay = true;
                    decide();
                    autoplay = was;
                }
            } else if (b.button == kSelect) {
                change_difficulty();
            }
        }
        // 皿・↑↓で上下（仕様 3.5）。押した瞬間は前の移動が終わっていれば 300ms、押し続けたら終わる 20ms 前から 70ms ずつ
        int hold_dir = 0;
        if (accept && !start_held) {
            if (input.button_held(kScratchB) || down_held) hold_dir = 1;  // 右回し（時計回り）で下へ
            else if (input.button_held(kScratchA) || up_held) hold_dir = -1;
        }
        if (hold_dir != 0) {
            if (hold_dir != prev_hold_dir) {
                if (t_ms > move_end) move(hold_dir, app.cfg.select_speed_first > 0 ? app.cfg.select_speed_first : 300);
            } else if (t_ms > move_end - 20) {
                move(hold_dir, app.cfg.select_speed_next > 0 ? app.cfg.select_speed_next : 70);
            }
        }
        prev_hold_dir = hold_dir;
        // 見た目のスクロール位置
        if (t_ms >= move_end) list_top = now_bar;
        else if (t_ms <= move_start) list_top = old_bar;
        else list_top = old_bar + static_cast<int64_t>((now_bar - old_bar) * (t_ms - move_start) / (move_end - move_start));
        if (t_ms < move_end) sctx.timers.set(10, t_ms);
        else sctx.timers.reset(10);
        {
            std::lock_guard<std::mutex> lock(mu);
            if (pending) {
                lib = pending;
                pending.reset();
                folder_charts.clear();
                rebuild();
            }
        }

        for (const std::string& nm : joystick_init().open_new()) {
            std::printf("コントローラー接続: %s\n", nm.c_str());
            joy_names.push_back(nm);
        }
        if (joystick_init().ready() && !joy_reported) {
            std::printf("コントローラーの準備が終わった（%dms）\n", joystick_init().took_ms());
            joy_reported = true;
        }
        // プレビュー
        stale_loads.erase(std::remove_if(stale_loads.begin(), stale_loads.end(),
                                         [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                          stale_loads.end());
        if (app.cfg.preview) {
            const Entry* pe = current();
            std::string want = pe && pe->kind == Entry::Kind::Chart ? pe->chart->path : "";
            if (!preview_path.empty() && want != preview_path) stop_preview(true);
            if (preview_load.valid() && preview_load.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                std::shared_ptr<Preview> p = preview_load.get();
                if (p && p->path == preview_path) {
                    app.mixer.stop();
                    app.mixer.wait_callback();
                    std::vector<ScheduledSound> schedule;
                    uint32_t rate = app.mixer.sample_rate();
                    auto frame_of = [&](int64_t us) { return us * static_cast<int64_t>(rate) / 1000000; };
                    for (const BgmEvent& b : p->chart.bgm) schedule.push_back({frame_of(b.time_us), b.sound});
                    for (const Note& n : p->chart.notes)
                        if ((n.kind == NoteKind::Normal || n.kind == NoteKind::Long) && n.sound)
                            schedule.push_back({frame_of(n.time_us), n.sound});
                    preview = p;
                    app.mixer.set_song(&preview->bank.samples(), std::move(schedule));
                    app.mixer.start(0);
                    app.mixer.fade_system_loop(0.025f, 1000);
                    std::printf("プレビュー: %s\n", preview->chart.meta.title.c_str());
                }
            }
            if (!want.empty() && preview_path.empty() && !preview_load.valid() && sctx.timers.lapse(11, t_ms) >= 500) {
                preview_path = want;
                uint32_t rate = app.mixer.sample_rate();
                preview_load = std::async(std::launch::async, [want, rate]() -> std::shared_ptr<Preview> {
                    BmsParseOptions popt;
                    popt.random_seed = 1;
                    BmsParseResult parsed = parse_bms_file(want, popt);
                    if (!parsed.ok) return nullptr;
                    auto p = std::make_shared<Preview>();
                    p->path = want;
                    p->chart = std::move(parsed.chart);
                    std::vector<std::string> missing;
                    std::vector<std::string> paths = resolve_chart_sounds(p->chart, missing);
                    p->bank.load(paths, rate, static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2)), nullptr);
                    return p;
                });
            }
        }

        // 描画
        if (sk) {
            const Level& lv = stack.back();
            const std::vector<Entry>& list = *lv.list;
            int n = static_cast<int>(list.size());
            const Entry* cur = current();
            sctx.type = 1, sctx.chart = nullptr, sctx.score = nullptr, sctx.course = nullptr;
            if (cur && cur->kind == Entry::Kind::Folder && cur->course_folder) {
                sctx.type = 6;
            } else if (cur && cur->kind == Entry::Kind::Course) {
                sctx.type = 8;
                sctx.course = cur->course.get();
                auto it = lib->scores.find(cur->course->hash);
                if (it != lib->scores.end()) sctx.score = &it->second;
            } else if (cur && cur->kind == Entry::Kind::Chart) {
                sctx.type = 0;
                sctx.chart = cur->chart;
                auto it = lib->scores.find(cur->chart->sha256);
                if (it != lib->scores.end()) sctx.score = &it->second;
            } else if (cur && cur->kind == Entry::Kind::Missing) {
                sctx.type = 0;
            }
            // 同じフォルダの、難易度ごとの譜面（WMIX の NORMAL / HYPER / ANOTHER の枠）
            sctx.diff_level.fill(-1), sctx.diff_count.fill(0), sctx.diff_lamp.fill(0);
            if (sctx.chart) {
                if (folder_charts.empty())
                    for (const ChartRecord& c : lib->charts) folder_charts[c.folder].push_back(&c);
                auto fit = folder_charts.find(sctx.chart->folder);
                if (fit != folder_charts.end())
                    for (const ChartRecord* c : fit->second) {
                        int d = c->difficulty - 1;
                        if (d < 0 || d > 4) continue;
                        // 同じ難易度が複数あれば、カーソルの譜面を優先する
                        if (sctx.diff_count[d]++ == 0 || c == sctx.chart) {
                            sctx.diff_level[d] = c->playlevel;
                            auto it = lib->scores.find(c->sha256);
                            sctx.diff_lamp[d] = it != lib->scores.end() ? it->second.clear : 0;
                        }
                    }
            }
            if (sk->reload_banner) {
                std::string want = sctx.chart ? banner_path_of(*sctx.chart) : "";
                if (want != banner_want) {
                    banner_want = want;
                    if (banner_tex) app.select_renderer->set_image_override(102, nullptr), SDL_DestroyTexture(banner_tex), banner_tex = nullptr;
                    if (!want.empty()) {
                        if (banner_thread.joinable()) banner_thread.join();
                        banner_thread = std::thread([b = banner, want] {
                            SkinImage img;
                            static const uint8_t black[3] = {0, 0, 0};
                            load_skin_image(want, img, black, false);
                            std::lock_guard<std::mutex> lock(b->mu);
                            b->path = want;
                            b->image = std::move(img);
                            b->ready = true;
                        });
                    }
                }
                std::lock_guard<std::mutex> lock(banner->mu);
                if (banner->ready) {
                    banner->ready = false;
                    // 読み終わったときにカーソルの曲が変わっていたら捨てる
                    if (banner->path == banner_want && !banner->image.rgba.empty() && !banner_tex) {
                        banner_tex = app.select_renderer->create_texture(banner->image);
                        app.select_renderer->set_image_override(102, banner_tex);
                    }
                }
            }
            sctx.has_banner = banner_tex != nullptr;
            sctx.has_text = false;
            if (sctx.chart) {
                auto [it, fresh] = folder_has_text.try_emplace(sctx.chart->folder, false);
                if (fresh) {
                    std::error_code ec;
                    for (const auto& de : fs::directory_iterator(sctx.chart->folder, ec)) {
                        std::string ext = de.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (ext == ".txt") it->second = true;
                    }
                }
                sctx.has_text = it->second;
            }
            sctx.folder_name = editing ? edit_buf + "|" : stack.size() > 1 ? lv.name : "検索語句を入力";
            sctx.panel = panel;
            sctx.difficulty_filter = difficulty_filter;
            sctx.mode_filter = mode_filter;
            sctx.sort_mode = sort_mode;
            sctx.mode_name = kModeNames[mode_filter];
            sctx.sort_name = kSortNames[sort_mode];
            sctx.autoplay = autoplay;
            sctx.windowed = !(SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN);
            sctx.list_count = n;
            int64_t total = static_cast<int64_t>(std::max(1, n)) * 1000;
            int lcb = static_cast<int>(((list_top % total) + total) % total);
            sctx.list_pos = lcb;
            SelectSkinState ss;
            ss.count = n;
            ss.lcb = lcb;
            ss.sel = sk->bar.center;
            ss.panel = panel;
            ss.mouse_x = mouse_x, ss.mouse_y = mouse_y;
            ss.mouse_idle_ms = t_ms - mouse_moved_ms;
            ss.readme = &readme_lines;
            ss.readme_open = readme_open;
            ss.readme_x = readme_x, ss.readme_y = readme_y;
            ss.bar = [&](int i) {
                SelectBar b;
                const Entry& en = list[i];
                if (en.kind == Entry::Kind::Folder) {
                    b.type = en.course_folder ? 6 : 1;
                    b.title = en.label;
                } else if (en.kind == Entry::Kind::Course) {
                    // コースのバーはレベルを出さず、ランプはコースの記録
                    b.type = 8;
                    b.title = en.label;
                    auto it = lib->scores.find(en.course->hash);
                    if (it != lib->scores.end()) b.lamp = it->second.clear;
                } else {
                    b.type = 0;
                    b.title = en.label;
                    if (en.kind == Entry::Kind::Chart) {
                        int df = en.chart->difficulty;
                        b.level_index = df >= 1 && df <= 5 ? df : 0;
                        b.level = en.chart->playlevel;
                        auto it = lib->scores.find(en.chart->sha256);
                        if (it != lib->scores.end()) b.lamp = it->second.clear;
                    }
                }
                return b;
            };
            PlaySkinState st;
            st.now_ms = t_ms;
            st.timers = &sctx.timers;
            st.option = [&](int o) { return sctx.option(o); };
            st.number = [&](int id) { return sctx.number(id); };
            st.bargraph = [&](int ty, double& v, double& m) { return sctx.bargraph(ty, v, m); };
            st.slider = [&](int ty, int& v, int& lo, int& hi) { return sctx.slider(ty, v, lo, hi); };
            st.text = [&](int id) { return sctx.text(id); };
            st.button = [&](int ty) { return sctx.button(ty); };
            st.select = &ss;
            // スキンがマウスカーソルを描くときは、OS のカーソルを隠す（二重に出ないように）
            bool skin_cursor = sk->mousecursor.src.valid && !sk->mousecursor.dst.empty();
            if (skin_cursor && SDL_CursorVisible()) SDL_HideCursor();
            else if (!skin_cursor && !SDL_CursorVisible()) SDL_ShowCursor();
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            app.select_renderer->draw_select(*sk, st);
        } else {
        SDL_SetRenderDrawColor(r, 12, 12, 20, 255);
        SDL_RenderClear(r);
        SDL_Color white{235, 235, 240, 255}, dim{140, 140, 155, 255}, accent{255, 210, 90, 255};
        std::string crumb;
        for (size_t i = 0; i < stack.size(); ++i) crumb += (i ? "  ›  " : "") + stack[i].name;
        text.draw(24, 16, 22, white, crumb, 0, 760);

        const Level& lv = stack.back();
        const std::vector<Entry>& list = *lv.list;
        int n = static_cast<int>(list.size());
        int rows = 15, top = std::clamp(lv.cursor - rows / 2, 0, std::max(0, n - rows));
        for (int i = 0; i < rows && top + i < n; ++i) {
            const Entry& en = list[top + i];
            float y = 60 + i * 40.0f;
            bool sel = top + i == lv.cursor;
            SDL_FRect bg{20, y, 780, 36};
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, sel ? 60 : 26, sel ? 56 : 26, sel ? 90 : 38, 255);
            SDL_RenderFillRect(r, &bg);
            SDL_Color badge_bg{50, 50, 70, 255};
            int lamp = 0;
            if (en.kind == Entry::Kind::Chart) {
                auto it = lib->scores.find(en.chart->sha256);
                if (it != lib->scores.end()) lamp = it->second.clear;
            }
            SDL_Color lc = lamp_color(lamp);
            SDL_FRect lampbar{20, y, 8, 36};
            SDL_SetRenderDrawColor(r, lc.r, lc.g, lc.b, 255);
            if (en.kind != Entry::Kind::Folder) SDL_RenderFillRect(r, &lampbar);
            if (!en.badge.empty() && en.kind != Entry::Kind::Folder) {
                SDL_FRect b{36, y + 6, 64, 24};
                SDL_SetRenderDrawColor(r, badge_bg.r, badge_bg.g, badge_bg.b, 255);
                SDL_RenderFillRect(r, &b);
                text.draw(68, y + 8, 18, accent, en.badge, 1, 60);
            }
            SDL_Color tc = en.kind == Entry::Kind::Missing ? dim : white;
            float tx = en.kind == Entry::Kind::Folder ? 40.0f : 112.0f;
            text.draw(tx, y + 3, 20, tc, en.label, 0, 560);
            if (en.kind == Entry::Kind::Folder) {
                char cnt[64];
                std::snprintf(cnt, sizeof(cnt), "%d / %d", en.owned, en.total);
                text.draw(790, y + 8, 18, dim, cnt, 2);
            } else {
                text.draw(tx + 4, y + 22, 13, dim, en.sub, 0, 520);
                if (en.kind == Entry::Kind::Missing) text.draw(790, y + 10, 15, dim, "未所持", 2);
            }
        }
        if (n == 0) text.draw(40, 80, 20, dim, "（空）");
        char pos[32];
        std::snprintf(pos, sizeof(pos), "%d / %d", n ? lv.cursor + 1 : 0, n);
        text.draw(800, 664, 16, dim, pos, 2);

        // 右側：選んでいる譜面の情報
        float px = 830;
        if (const Entry* cur = current()) {
            if (cur->kind == Entry::Kind::Chart) {
                const ChartRecord& c = *cur->chart;
                text.draw(px, 70, 24, white, c.title, 0, 420);
                if (!c.subtitle.empty()) text.draw(px, 100, 16, dim, c.subtitle, 0, 420);
                text.draw(px, 124, 17, white, c.artist, 0, 420);
                if (!c.subartist.empty()) text.draw(px, 146, 14, dim, c.subartist, 0, 420);
                char b[160];
                std::snprintf(b, sizeof(b), "GENRE  %s", c.genre.c_str());
                text.draw(px, 180, 16, dim, b, 0, 420);
                if (c.bpm_min != c.bpm_max) std::snprintf(b, sizeof(b), "BPM  %.0f - %.0f（主 %.0f）", c.bpm_min, c.bpm_max, c.bpm_main);
                else std::snprintf(b, sizeof(b), "BPM  %.0f", c.bpm_main);
                text.draw(px, 204, 16, white, b);
                std::snprintf(b, sizeof(b), "NOTES  %d%s   %d:%02d", c.notes, c.ln ? "（LNあり）" : "", c.length_ms / 60000,
                              c.length_ms / 1000 % 60);
                text.draw(px, 228, 16, white, b);
                std::snprintf(b, sizeof(b), "#RANK %d   TOTAL %d%s", c.rank, c.total, c.scratch ? "" : "   皿なし");
                text.draw(px, 252, 16, dim, b);
                ScoreRecord sc;
                auto it = lib->scores.find(c.sha256);
                if (it != lib->scores.end()) sc = it->second;
                text.draw(px, 300, 26, lamp_color(sc.clear).a ? lamp_color(sc.clear) : white, lamp_name(sc.clear));
                int max_ex = c.notes * 2;
                std::snprintf(b, sizeof(b), "EX SCORE  %d / %d（%.2f%%）", sc.exscore, max_ex,
                              max_ex ? sc.exscore * 100.0 / max_ex : 0.0);
                text.draw(px, 340, 17, white, b);
                if (sc.bp >= 0 && sc.playcount > 0) std::snprintf(b, sizeof(b), "BP  %d    MAX COMBO  %d", sc.bp, sc.max_combo);
                else std::snprintf(b, sizeof(b), "BP  -");
                text.draw(px, 366, 17, white, b);
                std::snprintf(b, sizeof(b), "PLAY  %d    CLEAR  %d", sc.playcount, sc.clearcount);
                text.draw(px, 392, 17, dim, b);
            } else if (cur->kind == Entry::Kind::Missing) {
                text.draw(px, 70, 24, dim, cur->missing.title, 0, 420);
                text.draw(px, 104, 17, dim, cur->missing.artist, 0, 420);
                text.draw(px, 140, 16, white, "この譜面は手元に無い");
                if (!cur->missing.url.empty()) text.draw(px, 168, 13, dim, "本体：" + cur->missing.url, 0, 420);
                if (!cur->missing.url_diff.empty()) text.draw(px, 188, 13, dim, "差分：" + cur->missing.url_diff, 0, 420);
            } else {
                text.draw(px, 70, 24, white, cur->label, 0, 420);
                if (!cur->sub.empty()) text.draw(px, 104, 15, dim, cur->sub, 0, 420);
                char b[64];
                std::snprintf(b, sizeof(b), "所持 %d / %d", cur->owned, cur->total);
                text.draw(px, 130, 17, white, b);
            }
        }
        static const char* gauges[] = {"GROOVE", "HARD", "DEATH", "EASY", "P-ATTACK", "G-ATTACK"};
        static const char* hs_fixes[] = {"OFF", "MAX BPM", "MIN BPM", "AVERAGE", "CONSTANT", "MAIN BPM"};
        static const char* effects[] = {"OFF", "HIDDEN", "SUDDEN", "HID+SUD"};
        char opt[192];
        std::snprintf(opt, sizeof(opt), "GAUGE %s   %s   HS %d.%02d  FIX %s", gauges[std::clamp(app.cfg.gauge, 0, 5)],
                      autoplay ? "AUTO PLAY" : "", app.cfg.hi_speed / 100, app.cfg.hi_speed % 100,
                      hs_fixes[std::clamp(app.cfg.hs_fix, 0, 5)]);
        text.draw(px, 552, 16, accent, opt);
        std::snprintf(opt, sizeof(opt), "LANE COVER %s %d   EFFECT %s", app.cfg.lane_cover ? "ON" : "OFF", app.cfg.lane_cover_v,
                      effects[std::clamp(app.cfg.effect, 0, 3)]);
        text.draw(px, 576, 16, accent, opt);
        std::string st_text;
        {
            std::lock_guard<std::mutex> lock(mu);
            st_text = busy ? "更新中…" : status;
        }
        text.draw(px, 644, 14, dim, st_text, 0, 420);
        // コントローラーの状態（Windows では準備に時間がかかることがある）
        std::string joy_text;
        if (!joystick_init().ready()) joy_text = "コントローラーを準備中…（キーボードは使える）";
        else if (joy_names.empty() && !raw_hid().devices().empty()) joy_text = "コントローラー：" + raw_hid().devices().front().name;
        else if (joy_names.empty()) joy_text = "コントローラー：なし";
        else joy_text = "コントローラー：" + joy_names.front() + (joy_names.size() > 1 ? " ほか" : "");
        if (!(SDL_GetWindowFlags(app.window) & SDL_WINDOW_INPUT_FOCUS)) joy_text = "ウィンドウをクリックすると操作できます";
        text.draw(px, 600, 14, joystick_init().ready() ? dim : accent, joy_text, 0, 420);
        text.draw(24, 690, 14, dim,
                  "↑↓ 選ぶ　Enter/→ 決定　Esc/← 戻る　START/Space オプション　F2 オート　F6 カバー　F1 キーコンフィグ　F5 更新　（コントローラ：白鍵 決定／黒鍵 戻る／皿 選ぶ）");
        if (std::getenv("BMJ_DEBUG_LOOP")) {
            // 開発用：選曲画面の1秒あたりの描画回数と、1回にかかった時間
            static uint64_t t = SDL_GetTicksNS();
            static int frames = 0;
            static double sum = 0, mx = 0;
            double ms = (SDL_GetTicksNS() - now) / 1e6;
            ++frames, sum += ms, mx = std::max(mx, ms);
            if (SDL_GetTicksNS() - t > 1000000000ull) {
                std::printf("選曲画面 %dfps、1回 平均%.2fms 最大%.2fms\n", frames, sum / frames, mx);
                frames = 0, sum = 0, mx = 0;
                t = SDL_GetTicksNS();
            }
        }
        if (panel == 1) draw_play_option_panel(r, text, app.cfg, (now - panel_open_ns) / 1e6);
        }  // 仮の画面
        bool settled;
        {
            std::lock_guard<std::mutex> lock(mu);
            settled = !busy && !pending;
        }
        if (const char* shot = std::getenv("BMJ_SELECT_SHOT"); shot && settled) {
            // 開発用：更新が終わってから1枚保存して終わる。BMJ_SELECT_KEYS の文字で操作してから撮る（d=下 e=決定）
            static size_t step = 0;
            const char* keys = std::getenv("BMJ_SELECT_KEYS");
            if (keys && keys[step]) {
                char k = keys[step++];
                if (k == 'd') move(1);
                else if (k == 'D') move(10);
                else if (k == 'u') move(-1);
                else if (k == 'e') decide();
                else if (k == 'b') back();
                else if (k == 'k') run_key_wizard(r, text, app.keycfg, keyconfig_path());
                else if (k == 'p') force_panel = true;
                else if (k == 's') change_difficulty();
                else if (k == 'z') run_skin_select(app, catalog), set_select_view();
                else if (k == '2') set_panel(2);
                else if (k == 't') catalog.select_type(0, app.cfg);
                else if (k == 'q') run_search("celestial");
                else if (k == 'h' && sk && !sk->helpfiles.empty()) open_readme({resolve_lr2_path(app.lr2_root, sk->helpfiles[0], false)});
                else if (k == 'o') sort_mode = (sort_mode + 1) % kSortCount, reapply_view();
                else if (k == 'a') autoplay = !autoplay;
                else if (k == '_') SDL_Delay(10);  // 待つだけ
            } else {
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
        }
        SDL_RenderPresent(r);
        SDL_DelayPrecise(4'000'000);
    }
    stop_preview(false);
    if (preview_load.valid()) preview_load.wait();
    for (auto& f : stale_loads) f.wait();
    app.mixer.fade_system_loop(1.0f, 0);
    app.mixer.stop_system(kSysSelect);
    if (banner_thread.joinable()) banner_thread.join();
    if (banner_tex) app.select_renderer->set_image_override(102, nullptr), SDL_DestroyTexture(banner_tex);
    if (worker.joinable()) worker.join();
}

}  // namespace bmj
