#include "app/select_scene.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <future>
#include "net/borrow.h"
#include "util/data_dir.h"
#include <condition_variable>
#include <functional>
#include <unordered_set>
#include <list>
#include <set>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

#include "app/lr2_import.h"
#include "app/play_session.h"
#include "audio/sound_bank.h"
#include "chart/bms_parser.h"
#include "chart/chart_loader.h"
#include "app/select_context.h"
#include "app/decide_scene.h"
#include "app/skin_select_scene.h"
#include "util/text.h"
#include "audio/system_sound.h"
#include "db/custom_folder.h"
#include "db/difficulty_table.h"
#include "db/folder_watch.h"
#include "game/gauge.h"
#include "game/lane_option.h"
#include "input/input.h"
#include "input/joystick_init.h"
#include "input/win_rawhid.h"
#include "mod/script.h"
#include "render/frame_hooks.h"
#include "render/keyconfig_scene.h"
#include "render/text_cache.h"
#include "util/dev.h"

namespace fs = std::filesystem;

namespace rd {

namespace {

// beatoraja と同じ並びのソートと鍵盤数（RD の拡張）
constexpr int kSortCount = 9, kModeCount = 8;
const char* const kSortNames[kSortCount] = {"OFF", "TITLE", "ARTIST", "BPM", "LENGTH", "LEVEL", "CLEAR", "SCORE", "MISSCOUNT"};
const char* const kModeNames[kModeCount] = {"ALL", "7KEY", "14KEY", "9KEY", "5KEY", "10KEY", "SINGLE", "DOUBLE"};
// 鍵盤数の絞り込みで、その譜面を出すか。RD の譜面は 7 鍵と 5 鍵だけ（持っていない譜面は 7 鍵として扱う）
bool mode_shows(int mode, int keys) {
    switch (mode) {
    case 0: case 6: return true;  // ALL・SINGLE
    case 1: return keys == 7;
    case 4: return keys == 5;
    default: return false;        // 14KEY・9KEY・10KEY・DOUBLE
    }
}

struct Entry;

// フォルダの中身。Entry を写しても中身は共有する（フォルダを開いて並べ替えるたびに、下の階層まで丸ごと写さない）。
// 共有をやめて写しを持たせるには detach()（メモリを節約しない設定のとき。前と同じ持ち方）
class EntryChildren {
public:
    bool empty() const;
    size_t size() const;
    const std::vector<Entry>* ptr() const;
    std::vector<Entry>& vec();
    std::vector<Entry>::const_iterator begin() const;
    std::vector<Entry>::const_iterator end() const;
    void push_back(Entry e);
    void push_front(Entry e);
    const Entry& back() const;
    void clear();
    void detach();

private:
    std::shared_ptr<std::vector<Entry>> p_;
};

struct Entry {
    // Header はフォルダの中の一番上に出す、そのフォルダの名前のバー（一番下の項目と一番上の項目の境目）。
    // ModBar は MOD が足したバー（ランダムセレクトなど。決定すると MOD が遊ぶ譜面を選ぶ）
    enum class Kind { Folder, Chart, Missing, Course, Header, ModBar } kind = Kind::Folder;
    std::string label, sub, badge;  // badge はレベルの表示（例：sl5）
    std::string note;                // 譜面のとき、アーティストの左に出す文字（MOD の label）
    const ChartRecord* chart = nullptr;
    std::shared_ptr<const TableChart> missing_info;  // 持っていない譜面のとき（ほかの要素は持たない。要素を小さくするため）
    std::shared_ptr<Course> course;  // コースのとき
    bool course_folder = false;      // コースを集めたフォルダ（バーの種類 6）
    // フォルダのバーの種類（LR2 の SRC_BAR_BODY の番号。スキンはこれで色を変える）：1 フォルダ、2 カスタムフォルダ、3 新曲フォルダ。
    // 難易度表は 1（フォルダ）、カスタムフォルダ（.lr2folder・MOD・FAVORITE）は 2 にして、色で見分けられるようにする
    int bar_type = 1;
    bool fs_folder = false;          // 曲フォルダの階層（中の譜面に LR2 の難易度の絞り込みをかける）
    bool keep_order = false;         // 中の譜面に並べ替えをかけない（MOD が keep_order を付けたフォルダ）
    // カスタムフォルダ。中身は開くとき（とフォルダのランプを出すとき）に集める（LR2 と同じ。全部を先に集めると、数が多いと遅い）。
    // #MAXTRACKS 1 のフォルダは、開くたびに 1 曲を選び直す
    const CustomFolderDef* custom = nullptr;
    bool custom_loaded = false;
    std::string dir;                 // 曲フォルダの階層のとき、そのフォルダのパス（F8 の読み直し）
    EntryChildren children;
    int owned = 0, total = 0;  // フォルダの中で持っている譜面の数
    std::shared_ptr<const ModBar> mod_info;  // ModBar のとき
    const TableChart& missing() const { return missing_info ? *missing_info : no_table_chart(); }
    const ModBar& mod_bar() const { return mod_info ? *mod_info : no_mod_bar(); }
    static const TableChart& no_table_chart() {
        static const TableChart t;
        return t;
    }
    static const ModBar& no_mod_bar() {
        static const ModBar b;
        return b;
    }
};

const std::vector<Entry>& no_entries() {
    static const std::vector<Entry> v;
    return v;
}
bool EntryChildren::empty() const { return !p_ || p_->empty(); }
size_t EntryChildren::size() const { return p_ ? p_->size() : 0; }
const std::vector<Entry>* EntryChildren::ptr() const { return p_ ? p_.get() : &no_entries(); }
std::vector<Entry>& EntryChildren::vec() {
    if (!p_) p_ = std::make_shared<std::vector<Entry>>();
    return *p_;
}
std::vector<Entry>::const_iterator EntryChildren::begin() const { return ptr()->begin(); }
std::vector<Entry>::const_iterator EntryChildren::end() const { return ptr()->end(); }
void EntryChildren::push_back(Entry e) { vec().push_back(std::move(e)); }
void EntryChildren::push_front(Entry e) { vec().insert(vec().begin(), std::move(e)); }
const Entry& EntryChildren::back() const { return p_->back(); }
void EntryChildren::clear() {
    if (p_) p_->clear();
}
void EntryChildren::detach() {
    if (p_) p_ = std::make_shared<std::vector<Entry>>(*p_);
}

// 記録の番号（MOD の組み合わせごと）。7k モードなら seven_key に 7k の記録の番号（それ以外は -1）
struct RecordProfile {
    int normal = 0;
    int seven_key = -1;
};

RecordProfile record_profile(SongDb& db, const App& app) {
    const std::string key = play_mod_key(app.active_mods());
    return {db.mod_profile(key), app.cfg.seven_key ? db.mod_profile(seven_key_record_key(key)) : -1};
}

// 曲の一覧（裏で読み直して、できたら差し替える）
struct Library {
    std::vector<std::string> roots;  // 登録した曲フォルダ
    std::vector<ChartRecord> charts;
    std::vector<DifficultyTable> tables;
    std::unordered_map<std::string, ScoreRecord> scores;  // sha256 → スコア
    // sha256 → カスタムランプの鍵 → ランプ（1 FAILED 2 クリア 3 FULL COMBO 4 PERFECT）
    std::unordered_map<std::string, std::unordered_map<std::string, int>> custom_lamps;
    RecordProfile profile;
    // 7k モードのとき、譜面の sha256 → 7k の記録を使うか（皿のある 7 鍵の譜面）。載っていないもの（コース）も 7k の記録
    std::unordered_map<std::string, bool> seven_key_sha;
    // その譜面（かコース）の記録の番号
    int record_profile_of(const std::string& sha256) const {
        if (profile.seven_key < 0) return profile.normal;
        auto it = seven_key_sha.find(sha256);
        return it == seven_key_sha.end() || it->second ? profile.seven_key : profile.normal;
    }
    // 記録（スコアとカスタムランプ）を読む。7k モードでは、皿のある 7 鍵の譜面とコースは 7k の記録、ほかはふだんの記録
    void load_records(const SongDb& db, int ln_mode, RecordProfile p) {
        profile = p;
        seven_key_sha.clear();
        if (p.seven_key >= 0)
            for (const ChartRecord& c : charts) seven_key_sha.emplace(c.sha256, c.scratch && c.keys == 7);
        scores.clear();
        custom_lamps.clear();
        for (int id : {p.normal, p.seven_key}) {
            if (id < 0) continue;
            for (const ScoreRecord& s : db.all_scores(ln_mode, id))
                if (record_profile_of(s.sha256) == id) scores[s.sha256] = s;
            for (const SongDb::CustomLampRow& r : db.all_custom_lamps(ln_mode, id))
                if (record_profile_of(r.sha256) == id) custom_lamps[r.sha256][r.gauge] = r.lamp;
        }
    }
    // カスタムフォルダ（.lr2folder）。中身は曲と記録から作った LR2 の形の表で集める
    std::vector<CustomFolderNode> custom_nodes;
    std::shared_ptr<CustomFolderQuery> custom_query;
    // MOD が作ったフォルダ（仕様書14章「フォルダの作成」）
    std::vector<ModFolder> mod_folders;
    // 譜面の傾向（使う MOD があるときに初めて読む。記録を読み直すときは前のものを使い回す）
    mutable std::shared_ptr<const std::unordered_map<std::string, ChartAttr>> attrs;
    // md5 → 譜面（カスタムフォルダの中身を集めるときに、初めて要ったら作る）
    std::unordered_map<std::string, const ChartRecord*> md5_index;
    // 曲フォルダ → その中の譜面（難易度ごとの枠に使う。初めて要ったら作る）
    std::unordered_map<std::string, std::vector<const ChartRecord*>> by_folder;
    // 譜面ごとに、同じ sha256 の最初の譜面の番号（フォルダの記録の合計で、同じ譜面を 2 度数えないため）
    std::vector<int> same_sha;
    // md5 → 入っている難易度表のレベル（「★12 sl5」。表の順。初めて要ったら作る）
    mutable std::unordered_map<std::string, std::string> table_levels_;
    const std::string& table_levels(const std::string& md5) const {
        static const std::string none;
        if (table_levels_.empty()) {
            table_levels_.emplace("", "");
            for (const DifficultyTable& t : tables)
                for (const TableChart& tc : t.charts) {
                    if (tc.md5.empty()) continue;
                    std::string& s = table_levels_[tc.md5];
                    const std::string lv = t.symbol + tc.level;
                    if ((" " + s + " ").find(" " + lv + " ") == std::string::npos) s += (s.empty() ? "" : " ") + lv;
                }
        }
        auto it = table_levels_.find(md5);
        return it != table_levels_.end() ? it->second : none;
    }
    // 表で初めて作ると全部の譜面をなめて止まるので、裏で作れるときは先に作っておく
    void build_indexes() {
        table_levels("");
        if (md5_index.empty())
            for (const ChartRecord& c : charts) md5_index.emplace(c.md5, &c);
        if (by_folder.empty())
            for (const ChartRecord& c : charts) by_folder[c.folder].push_back(&c);
        if (same_sha.size() != charts.size()) {
            std::unordered_map<std::string_view, int> first;
            first.reserve(charts.size());
            same_sha.resize(charts.size());
            for (size_t i = 0; i < charts.size(); ++i) same_sha[i] = first.try_emplace(charts[i].sha256, static_cast<int>(i)).first->second;
        }
    }
};

Entry chart_entry_of(const ChartRecord* c, const std::string& badge) {
    Entry e;
    e.kind = Entry::Kind::Chart;
    e.chart = c;
    e.label = c->title + (c->subtitle.empty() ? "" : " " + c->subtitle);
    e.sub = c->artist;
    e.badge = badge;
    return e;
}

// カスタムフォルダの中身を集める（e は一覧の中のフォルダ。中身を差し替える）
// md5 の並びから、カスタムフォルダの中身を作る
void fill_custom_entry(Library& lib, const Entry& e, const std::vector<std::string>& md5s) {
    Entry& m = const_cast<Entry&>(e);
    m.children.clear();
    m.owned = m.total = 0;
    m.custom_loaded = true;
    if (lib.md5_index.empty())
        for (const ChartRecord& c : lib.charts) lib.md5_index.emplace(c.md5, &c);
    for (const std::string& md5 : md5s) {
        auto it = lib.md5_index.find(md5);
        if (it == lib.md5_index.end()) continue;
        m.children.push_back(chart_entry_of(it->second, std::to_string(it->second->playlevel)));
        m.owned++, m.total++;
    }
}

void load_custom_entry(Library& lib, const Entry& e) {
    if (!e.custom || !lib.custom_query) {
        fill_custom_entry(lib, e, {});
        return;
    }
    fill_custom_entry(lib, e, lib.custom_query->run(*e.custom));
}

// MOD のフォルダを作る。譜面の傾向を使う MOD があれば、分析した傾向と、譜面が入っている難易度表のレベルも渡す
std::vector<ModFolder> make_mod_folders(const Library& lib, const std::vector<const ModManifest*>& mods, std::vector<std::string>* errors) {
    std::unordered_map<std::string, std::vector<ModTableLevel>> tables;
    ModChartExtra extra;
    if (std::any_of(mods.begin(), mods.end(), [](const ModManifest* m) { return m->chart_attr; })) {
        if (!lib.attrs) {
            SongDb rd;
            auto a = std::make_shared<std::unordered_map<std::string, ChartAttr>>();
            if (rd.open(song_db_path())) *a = rd.chart_attrs();
            lib.attrs = std::move(a);
            if (dev_env("RD_TIMING")) std::printf("      MOD：譜面の傾向を読んだ（%zu 件）\n", lib.attrs->size());
        }
        for (size_t ti = 0; ti < lib.tables.size(); ++ti) {
            const DifficultyTable& t = lib.tables[ti];
            std::unordered_map<std::string, int> order;
            for (size_t li = 0; li < t.levels.size(); ++li) order.emplace(t.levels[li], static_cast<int>(li));
            for (const TableChart& tc : t.charts) {
                if (tc.md5.empty()) continue;
                auto& v = tables[tc.md5];
                const int lo = order.count(tc.level) ? order[tc.level] : static_cast<int>(t.levels.size());
                if (std::none_of(v.begin(), v.end(), [&](const ModTableLevel& x) { return x.table_order == static_cast<int>(ti) && x.level == tc.level; }))
                    v.push_back({t.name.empty() ? t.url : t.name, t.symbol, tc.level, static_cast<int>(ti), lo});
            }
        }
        extra.attrs = lib.attrs.get(), extra.tables = &tables;
    }
    return build_mod_folders(mods, lib.charts, lib.scores, mod_data_dir(""), errors, extra, mod_cache_dir());
}

// カスタムフォルダを読み、中身を集める表を作り直す（曲・記録・お気に入りが変わったとき）
// MOD のフォルダも、ここで作り直す
void load_custom(Library& lib, const std::vector<const ModManifest*>& mods, const std::vector<std::string>& custom_dirs,
                 const std::unordered_map<std::string, int>& favs) {
    // 開発用：RD_TIMING があれば内訳を出す
    uint64_t lap_ns = SDL_GetTicksNS();
    auto lap = [&](const char* what) {
        if (!dev_env("RD_TIMING")) return;
        uint64_t now = SDL_GetTicksNS();
        std::printf("    %s %.0f ms\n", what, (now - lap_ns) / 1e6);
        lap_ns = now;
    };
    std::vector<std::string> errors;
    lib.mod_folders = make_mod_folders(lib, mods, &errors);
    lap("MOD のフォルダ");
    for (const std::string& e : errors) std::printf("MOD のフォルダを作れない：%s\n", e.c_str());
    lib.custom_nodes.clear();
    for (const std::string& d : custom_dirs) {
        std::vector<CustomFolderNode> n = load_custom_folders(d);
        lib.custom_nodes.insert(lib.custom_nodes.end(), n.begin(), n.end());
    }
    if (lib.custom_nodes.empty()) {
        lib.custom_query.reset();
        return;
    }
    auto q = std::make_shared<CustomFolderQuery>();
    lap("カスタムフォルダを読む");
    if (q->build(lib.charts, lib.scores, favs)) lib.custom_query = q;
    lap("カスタムフォルダの表を作る");
}

void load_custom(Library& lib, App& app, const std::unordered_map<std::string, int>& favs) {
    load_custom(lib, app.active_mods(), app.cfg.custom_folder_dirs, favs);
}

// 分類のフォルダの名前。folderinfo.txt があればその #TITLE（LR2: songmanage:1391）、無ければディレクトリ名。
// 一覧を作り直すたびに読まないよう、フォルダごとに覚えておく
std::string folder_title(const std::string& dir) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::string> memo;
    std::lock_guard<std::mutex> lock(mu);
    auto [it, fresh] = memo.try_emplace(dir);
    if (fresh) {
        it->second = fs::path(dir).filename().string();
        std::ifstream f(fs::path(dir) / "folderinfo.txt", std::ios::binary);
        if (f) {
            std::string text = to_utf8_auto(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                std::string l(trim(line));
                if (to_upper_ascii(l).rfind("#TITLE", 0) == 0 && l.size() > 6) {
                    std::string t(trim(std::string_view(l).substr(6)));
                    if (!t.empty()) it->second = t;
                }
            }
        }
    }
    return it->second;
}

// 音の設定のスライダー（EQ 10〜16、音量 17〜19、エフェクタ 20〜25、ピッチ 26）
bool is_sound_slider(int ty) { return ty >= 10 && ty <= 26; }

// 新曲とみなす境の時刻（LR2: titleflash 時間より後に見つけた譜面）
int64_t new_song_since(int hours) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now - static_cast<int64_t>(hours) * 3600;
}

// 判定の欄（ボタン 900）のコマ：0 = OFF、1〜4 = 本体の判定（VERY HARD … EASY）、5〜13 = 一覧の判定（JUDGE1〜9）
int judge_choice(const ModManifest& m) {
    const int ns = static_cast<int>(std::size(kStandardJudgeIds));
    for (int i = 0; i < ns; ++i)
        if (m.judge_use == kStandardJudgeIds[i]) return i + 1;
    for (size_t i = 0; i < m.judges.size() && i < 9; ++i)
        if (m.judges[i].id == m.judge_use) return ns + 1 + static_cast<int>(i);
    return 0;
}

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
// new_since：この時刻より後に見つけた譜面を NEW SONG に集める。favs：md5 → 1 = FAVORITE 2 = IGNORE。
// IGNORE の譜面は IGNORE のフォルダ以外には出さない（LR2: songmanage:821）
std::vector<Entry> build_tree(const Library& lib, int64_t new_since, const std::unordered_map<std::string, int>& favs) {
    auto fav_of = [&](const std::string& md5) {
        auto it = favs.find(md5);
        return it != favs.end() ? it->second : 0;
    };
    std::unordered_map<std::string, const ChartRecord*> by_md5, by_sha256;
    for (const ChartRecord& c : lib.charts) by_md5.emplace(c.md5, &c), by_sha256.emplace(c.sha256, &c);
    // 開発用：RD_TIMING があれば、一覧づくりの内訳を出す
    uint64_t lap_ns = SDL_GetTicksNS();
    auto lap = [&](const char* what) {
        if (!dev_env("RD_TIMING")) return;
        uint64_t now = SDL_GetTicksNS();
        std::printf("    一覧：%s %.0f ms\n", what, (now - lap_ns) / 1e6);
        lap_ns = now;
    };
    lap("索引");
    auto chart_entry = [&](const ChartRecord* c, const std::string& badge) { return chart_entry_of(c, badge); };
    std::vector<Entry> root;
    for (const DifficultyTable& t : lib.tables) {
        Entry tf;
        tf.label = t.name.empty() ? t.url : t.name;
        tf.sub = t.warning.empty() ? (t.from_cache ? "手元の版" : "") : t.warning;
        // 先頭に、表の全部の譜面を 1 つに並べた「全曲」のフォルダを置く（表の順。同じ譜面は 1 回だけ）
        Entry all;
        all.label = "全曲";
        std::set<std::string> all_seen;
        for (const std::string& lv : t.levels) {
            Entry lf;
            lf.badge = t.symbol + lv;
            lf.label = t.symbol + lv;
            std::set<std::string> seen;  // 同じ譜面は 1 回だけ（LR2 と同じ）
            for (const TableChart& tc : t.charts) {
                if (tc.level != lv) continue;
                if (!tc.md5.empty() && !seen.insert(tc.md5).second) continue;
                auto it = by_md5.find(tc.md5);
                if (it != by_md5.end() && fav_of(tc.md5) == 2) continue;
                if (it != by_md5.end()) {
                    lf.children.push_back(chart_entry(it->second, lf.badge));
                    lf.owned++;
                } else {
                    Entry m;
                    m.kind = Entry::Kind::Missing;
                    m.missing_info = std::make_shared<const TableChart>(tc);
                    m.label = tc.title;
                    m.sub = tc.artist;
                    m.badge = lf.badge;
                    lf.children.push_back(m);
                }
                lf.total++;
                if (tc.md5.empty() || all_seen.insert(tc.md5).second) {
                    all.children.push_back(lf.children.back());
                    all.owned += lf.children.back().kind == Entry::Kind::Chart;
                    all.total++;
                }
            }
            tf.owned += lf.owned;
            tf.total += lf.total;
            tf.children.push_back(std::move(lf));
        }
        if (!all.children.empty()) tf.children.push_front(std::move(all));
        root.push_back(std::move(tf));
    }
    // コースのフォルダ（LR2 と同じく一番上の階層に、COURSE と 段位認定 を必ず出す）。
    // 中身は難易度表に書かれたコース。段位の制約があれば段位認定、無ければ COURSE に入れる
    // 中は、コースの組（GENOCIDE 2018・NEW GENERATION・Satellite Skill Analyzer など）ごとのフォルダに分ける
    // （全部を 1 つに並べると、段位が多くて選びにくい）。同じ組が 2 つの表に載っていれば（Stella と Stella Sub など）1 つにまとめる
    Entry expert, grade;
    expert.label = "COURSE", grade.label = "段位認定";
    expert.course_folder = grade.course_folder = true;
    std::vector<std::pair<std::string, Entry>> groups[2];  // 0 = COURSE、1 = 段位認定。組の名前 → フォルダ（出てきた順）
    std::set<std::string> seen_groups;                      // 中身（コースの名前の並び）で、同じ組を二度入れない
    std::set<std::string> seen_courses;                     // 課題曲の並びと制約で、同じコースを二度入れない（難易度表のものを先に使う）
    // LR2 から引き継いだ段位・コースは、難易度表のあとに並べる（起動のあいだは同じものを使う）
    static const std::vector<TableCourse> lr2_courses = read_lr2_courses(lr2_courses_path());
    struct CourseSource {
        std::string name;
        const std::vector<TableCourse>* courses;
    };
    std::vector<CourseSource> sources;
    for (const DifficultyTable& t : lib.tables) sources.push_back({t.name.empty() ? t.url : t.name, &t.courses});
    sources.push_back({"LR2", &lr2_courses});
    for (const CourseSource& src : sources) {
        std::map<int, std::vector<const TableCourse*>> by_group;
        for (const TableCourse& tc : *src.courses) {
            std::string songs;
            for (const TableChart& c : tc.charts) songs += c.md5 + c.sha256 + ',';
            // 課題曲が同じでも制約が違えば別のコース（GENOCIDE 2018 の No Good・ノースピなど）
            for (const std::string& k : tc.constraints)
                if (k != "grade_mirror" && k != "gauge_lr2" && k != "grade") songs += '|' + k;
            if (seen_courses.insert(songs).second) by_group[tc.group].push_back(&tc);
        }
        for (auto& [gi, list] : by_group) {
            std::string sig;
            for (const TableCourse* tc : list) sig += tc->name + '\n';
            if (!seen_groups.insert(sig).second) continue;
            const std::string label = course_group_label(list, src.name);
            for (const TableCourse* tc : list) {
                Entry ce;
                ce.kind = Entry::Kind::Course;
                ce.course = std::make_shared<Course>(make_table_course(*tc, by_md5, by_sha256));
                ce.label = ce.course->name;
                auto& folders = groups[ce.course->type == 2 ? 1 : 0];
                auto it = std::find_if(folders.begin(), folders.end(), [&](const auto& g) { return g.first == label; });
                if (it == folders.end()) {
                    folders.push_back({label, Entry{}});
                    it = folders.end() - 1;
                    it->second.course_folder = true, it->second.label = label;
                }
                it->second.owned += ce.course->playable();
                it->second.total++;
                it->second.children.push_back(std::move(ce));
            }
        }
    }
    for (int k = 0; k < 2; ++k)
        for (auto& [label, f] : groups[k]) {
            Entry& parent = k == 1 ? grade : expert;
            parent.owned += f.owned, parent.total += f.total;
            parent.children.push_back(std::move(f));
        }
    root.push_back(std::move(expert));
    root.push_back(std::move(grade));
    lap("難易度表とコース");
    // 曲フォルダ（LR2 と同じ。docs/lr2_songlist_spec.md 2〜4 章）：登録した曲フォルダごとに 1 つ、実際の階層のとおり。
    // フォルダを開いたとき、直下に曲フォルダ（譜面の入ったフォルダ）が 1 つでもあれば、その全部の譜面（差分）を 1 譜面 1 バーで並べ、
    // 曲フォルダ自体のバーは出さない。無ければ直下のフォルダを並べる
    auto upper = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    };
    // 並べるときのキー：LR2 と同じく、パスを Shift_JIS にして英字を大文字にしたもの（バイト順で比べる）
    std::unordered_map<std::string, std::string> sort_key_memo;
    auto sort_key = [&](const std::string& p) -> const std::string& {
        auto [it, fresh] = sort_key_memo.try_emplace(p);
        if (fresh) {
            std::string k = utf8_to_cp932(p);
            // 2 バイト文字の 2 バイト目は大文字にしない
            for (size_t i = 0; i < k.size(); ++i) {
                unsigned char ch = static_cast<unsigned char>(k[i]);
                if ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC)) {
                    ++i;
                    continue;
                }
                if (ch >= 'a' && ch <= 'z') k[i] = static_cast<char>(ch - 32);
            }
            it->second = std::move(k);
        }
        return it->second;
    };
    auto norm = [&](const std::string& d) {
        std::string s = fs::path(d).lexically_normal().string();
        while (s.size() > 3 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        return s;
    };
    std::unordered_map<std::string, std::vector<const ChartRecord*>> charts_in;  // フォルダ → 直下の譜面
    std::unordered_map<std::string, std::set<std::string>> subdirs;               // フォルダ → 譜面のある下のフォルダ
    std::set<std::string> roots;
    for (const std::string& r : lib.roots) roots.insert(upper(norm(r)));
    // 同じフォルダの譜面は並んでいることが多いので、直前のフォルダと同じなら整え直さない。
    // 親をたどるのは、フォルダごとに 1 回だけ
    std::string prev_raw, prev_norm;
    std::unordered_set<std::string> linked;
    for (const ChartRecord& c : lib.charts) {
        if (fav_of(c.md5) == 2) continue;
        if (c.folder != prev_raw) prev_raw = c.folder, prev_norm = norm(c.folder);
        const std::string& f = prev_norm;
        charts_in[f].push_back(&c);
        if (!linked.insert(f).second) continue;
        // 登録した曲フォルダまで上がって、親子をつなぐ（どの曲フォルダの下でもなければ出さない）
        std::string child = f;
        for (int guard = 0; guard < 64; ++guard) {
            if (roots.count(upper(child))) break;
            std::string parent = norm(fs::path(child).parent_path().string());
            if (parent == child || parent.empty()) break;
            subdirs[parent].insert(child);
            child = parent;
        }
    }
    lap("曲フォルダの親子");
    auto by_path = [&](const std::string& a, const std::string& b) {
        const std::string& ka = sort_key(a);
        const std::string& kb = sort_key(b);
        return std::lexicographical_compare(ka.begin(), ka.end(), kb.begin(), kb.end(),
                                            [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
    };
    std::function<void(Entry&, const std::string&)> fill = [&](Entry& e, const std::string& dir) {
        std::vector<std::string> kids(subdirs[dir].begin(), subdirs[dir].end());
        std::sort(kids.begin(), kids.end(), by_path);
        std::vector<const ChartRecord*> list;
        for (const std::string& k : kids)
            if (auto it = charts_in.find(k); it != charts_in.end()) list.insert(list.end(), it->second.begin(), it->second.end());
        if (!list.empty()) {
            // 並び：曲フォルダのパス順 → レベル順（INSANE は最後）。同じ譜面（md5）は 1 回だけ
            std::stable_sort(list.begin(), list.end(), [&](const ChartRecord* a, const ChartRecord* b) {
                if (a->folder != b->folder) return by_path(a->folder, b->folder);
                int la = a->playlevel + (a->difficulty == 5 ? 100 : 0), lb = b->playlevel + (b->difficulty == 5 ? 100 : 0);
                if (la != lb) return la < lb;
                return a->difficulty < b->difficulty;
            });
            std::set<std::string> seen;
            for (const ChartRecord* c : list) {
                if (!seen.insert(c->md5).second) continue;
                e.children.push_back(chart_entry(c, std::to_string(c->playlevel)));
                e.owned++, e.total++;
            }
            return;
        }
        for (const std::string& k : kids) {
            Entry f;
            f.fs_folder = true;
            f.dir = k;
            f.label = folder_title(k);
            fill(f, k);
            if (f.children.empty()) continue;
            e.owned += f.owned, e.total += f.total;
            e.children.push_back(std::move(f));
        }
    };
    std::vector<std::string> root_dirs(lib.roots.begin(), lib.roots.end());
    std::sort(root_dirs.begin(), root_dirs.end(), by_path);
    std::vector<Entry> song_roots;
    for (const std::string& r : root_dirs) {
        Entry f;
        f.fs_folder = true;
        std::string d = norm(r);
        f.dir = d;
        f.label = fs::path(d).filename().string();
        if (f.label.empty()) f.label = d;
        fill(f, d);
        song_roots.push_back(std::move(f));
    }
    lap("曲フォルダの並べ替え");
    // カスタムフォルダ（LR2: 登録した曲フォルダの次）。#COMMAND の結果を並べる（難易度の絞り込みはかけない）
    auto custom_entry = [&](const CustomFolderDef& d) {
        Entry f;
        f.bar_type = 2;
        f.label = d.title;
        f.custom = &d;  // 中身は開くときに集める（load_custom_entry）
        return f;
    };
    for (const CustomFolderNode& n : lib.custom_nodes) {
        if (!n.group) {
            song_roots.push_back(custom_entry(n.defs.front()));
            continue;
        }
        Entry g;
        g.bar_type = 2;
        g.label = n.title;
        for (const CustomFolderDef& d : n.defs) {
            Entry f = custom_entry(d);
            g.owned += f.owned, g.total += f.total;
            g.children.push_back(std::move(f));
        }
        song_roots.push_back(std::move(g));
    }
    // MOD のフォルダ（カスタムフォルダの後ろ）。MOD が label を付けた譜面は、アーティストの左に出す
    std::function<Entry(const ModFolder&)> mod_entry = [&](const ModFolder& mf) {
        Entry f;
        f.bar_type = 2;
        f.label = mf.title;
        f.keep_order = mf.keep_order;
        for (const ModFolder& c : mf.children) {
            Entry sub = mod_entry(c);
            f.owned += sub.owned, f.total += sub.total;
            f.children.push_back(std::move(sub));
        }
        for (size_t i = 0; i < mf.charts.size(); ++i) {
            auto it = by_sha256.find(mf.charts[i]);
            if (it == by_sha256.end() || fav_of(it->second->md5) == 2) continue;
            Entry c = chart_entry(it->second, std::to_string(it->second->playlevel));
            if (i < mf.labels.size()) c.note = mf.labels[i];
            f.children.push_back(std::move(c));
            f.owned++, f.total++;
        }
        return f;
    };
    lap("カスタムフォルダ");
    for (const ModFolder& mf : lib.mod_folders) song_roots.push_back(mod_entry(mf));
    // 並び：難易度表 → 登録した曲フォルダ → カスタムフォルダ → MOD のフォルダ → NEW SONG → COURSE / 段位認定（LR2 のルートと同じく、コースのフォルダは後ろ）
    std::vector<Entry> courses(std::make_move_iterator(root.end() - 2), std::make_move_iterator(root.end()));
    root.erase(root.end() - 2, root.end());
    for (Entry& f : song_roots) root.push_back(std::move(f));
    {
        // NEW SONG（LR2: __NEWSONG__）。新しく見つけた譜面があるときだけ出す。曲フォルダと同じ並び
        std::vector<const ChartRecord*> fresh;
        std::set<std::string> seen;
        for (const ChartRecord& c : lib.charts)
            if (c.added > 0 && c.added > new_since && fav_of(c.md5) != 2 && seen.insert(c.md5).second) fresh.push_back(&c);
        if (!fresh.empty()) {
            std::stable_sort(fresh.begin(), fresh.end(), [&](const ChartRecord* a, const ChartRecord* b) {
                if (a->folder != b->folder) return by_path(a->folder, b->folder);
                return a->playlevel < b->playlevel;
            });
            Entry f;
            f.label = "NEW SONG";
            f.bar_type = 3;
            f.fs_folder = true;
            for (const ChartRecord* c : fresh) f.children.push_back(chart_entry(c, std::to_string(c->playlevel))), f.owned++, f.total++;
            root.push_back(std::move(f));
        }
        // FAVORITE / IGNORE（LR2 の既定のカスタムフォルダ favorite・ignore。印を付けた譜面があるときだけ出す）。
        // 同じものを集めるカスタムフォルダ（favorite = 1 / 2）を置いていれば、そちらに任せる
        bool custom_fav[3] = {};
        for (const CustomFolderNode& n : lib.custom_nodes)
            for (const CustomFolderDef& d : n.defs)
                for (int v : {1, 2})
                    if (d.command.find("favorite = " + std::to_string(v)) != std::string::npos) custom_fav[v] = true;
        for (int v : {1, 2}) {
            if (custom_fav[v]) continue;
            std::vector<const ChartRecord*> list;
            std::set<std::string> seen2;
            for (const ChartRecord& c : lib.charts)
                if (fav_of(c.md5) == v && seen2.insert(c.md5).second) list.push_back(&c);
            if (list.empty()) continue;
            std::stable_sort(list.begin(), list.end(), [&](const ChartRecord* a, const ChartRecord* b) { return by_path(a->folder, b->folder); });
            Entry f;
            f.bar_type = 2;
            f.label = v == 1 ? "FAVORITE" : "IGNORE";
            for (const ChartRecord* c : list) f.children.push_back(chart_entry(c, std::to_string(c->playlevel))), f.owned++, f.total++;
            root.push_back(std::move(f));
        }
    }
    for (Entry& f : courses) root.push_back(std::move(f));
    return root;
}

// 曲の一覧を読む（登録した曲フォルダ・譜面と、難易度の補い）
// 選曲画面に出す譜面（DP の譜面は、持っている譜面として曲データベースに載せるが、遊べないので出さない）
std::vector<ChartRecord> playable_charts(const SongDb& db) {
    std::vector<ChartRecord> v = db.all_charts();
    v.erase(std::remove_if(v.begin(), v.end(), [](const ChartRecord& c) { return c.keys >= 10; }), v.end());
    return v;
}

void load_charts(Library& lib, const SongDb& db, const AppConfig& cfg) {
    lib.roots = cfg.song_dirs;
    // 開発用：RD_TIMING があれば内訳を出す
    uint64_t lap_ns = SDL_GetTicksNS();
    auto lap = [&](const char* what) {
        if (!dev_env("RD_TIMING")) return;
        uint64_t now = SDL_GetTicksNS();
        std::printf("    %s %.0f ms\n", what, (now - lap_ns) / 1e6);
        lap_ns = now;
    };
    lib.charts = playable_charts(db);
    lap("譜面：曲データベースから読む");
    // 登録した曲フォルダそのものが無い（ドライブが無い・別の PC へ持ってきた）ときは、その中の譜面を持っていない扱いにする。
    // 曲データベースからは消さない（つなぎ直せば戻る）。見るのは曲フォルダの 1 段目だけ（譜面ごとには確かめない）
    std::vector<std::string> gone;
    // 止まっている外付けの HDD は、回り始めるまで数秒答えない。無いドライブはすぐに「無い」と答えるので、
    // 全部の曲フォルダを同時に確かめ、1.5 秒で答えの無いものは「ある」とみなす（起動のたびに HDD が回るのを待たない）
    std::vector<std::future<bool>> answers;
    for (const std::string& r : cfg.song_dirs) {
        auto answer = std::make_shared<std::promise<bool>>();
        answers.push_back(answer->get_future());
        std::thread([answer, r] {
            std::error_code ec;
            answer->set_value(fs::is_directory(fs::u8path(r), ec));
        }).detach();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    for (size_t i = 0; i < cfg.song_dirs.size(); ++i) {
        const std::string& r = cfg.song_dirs[i];
        const bool exists = answers[i].wait_until(deadline) != std::future_status::ready || answers[i].get();
        if (!exists) {
            std::string p = fs::u8path(r).lexically_normal().string();
            if (!p.empty() && p.back() != '/' && p.back() != '\\') p += static_cast<char>(fs::path::preferred_separator);
            gone.push_back(p);
            std::printf("曲フォルダが無い（中の譜面は持っていない扱いにする）: %s\n", r.c_str());
        }
    }
    if (!gone.empty()) {
        const size_t before = lib.charts.size();
        lib.charts.erase(std::remove_if(lib.charts.begin(), lib.charts.end(),
                                        [&](const ChartRecord& c) {
                                            for (const std::string& g : gone)
                                                if (c.path.size() > g.size() && c.path.compare(0, g.size(), g) == 0) return true;
                                            return false;
                                        }),
                         lib.charts.end());
        std::printf("無い曲フォルダの譜面 %zu 件を外した\n", before - lib.charts.size());
    }
    lap("譜面：曲フォルダがあるか確かめる");
    fill_difficulties(lib.charts);
    lap("譜面：難易度を埋める");
}


// プレイオプションのパネルを開いている間の鍵盤（LR2: Songselect.cpp:1350 SetPlayOption）。
//   1=鍵盤数の絞り込み 2=RANDOM 3=BATTLE 4=GAUGE 5=HS- 7=HS+ 6=7k モード（LR2 は ASSIST） 6+7=HIDDEN/SUDDEN 5+7=HS-FIX
// RD にまだ無いもの（BATTLE）は何もしない。変えたら true
bool play_option_keys(App& app, const InputMapper& input, const std::vector<ButtonInput>& buttons) {
    AppConfig& cfg = app.cfg;
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
    if (pressed[1]) loop(0, 7, 1, cfg.select_mode), changed = true;  // 鍵盤数の絞り込み
    if (pressed[4]) app.cycle_gauge(1), changed = true;  // ゲージ（使っている MOD のカスタムゲージも並ぶ）
    if (pressed[2]) loop(0, 5, 1, cfg.random), changed = true;  // RANDOM
    if (pressed[6] && !down(7)) cfg.seven_key = !cfg.seven_key, changed = true;  // 7k モード（ASSIST の場所）
    if ((pressed[6] && held(7)) || (pressed[7] && held(6))) loop(0, 3, -1, cfg.effect), changed = true;
    if (pressed[5] && !down(7) && !down(6)) loop(cfg.hs_min, cfg.hs_max, -cfg.hs_margin, cfg.hi_speed), changed = true;
    if (pressed[7] && !down(5) && !down(6)) loop(cfg.hs_min, cfg.hs_max, cfg.hs_margin, cfg.hi_speed), changed = true;
    if ((pressed[5] && held(7)) || (pressed[7] && held(5))) {
        // HS-FIX。5 の MAIN BPM は OpenLR2 の拡張
        loop(0, 5, 1, cfg.hs_fix);
        changed = true;
    }
    if (pressed[3]) changed = true;  // BATTLE（未対応。LR2 と同じく音だけ鳴る）
    // ターゲット：SELECT か皿（片方の向き）で次へ（LR2: SetBmsFilter。キーボードの ←→ は別に受ける）。IR の 6〜8 は無い
    for (const ButtonInput& b : buttons)
        if (b.down && (b.button == kSelect || b.button == kScratchA)) loop(0, 5, 1, cfg.target), changed = true;
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
                   {"HI-SPEED −", hs, true}, {"MODE", cfg.seven_key ? "7K" : "7K+S", true}, {"HI-SPEED ＋", hs, true}};
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

// 選曲スキンを読むときの条件（選曲画面に入った直後の状態）
std::array<bool, 1000> select_load_options(App& app, OptionNames& names) {
    names.load(app.lr2_root);
    SelectContext sctx;
    sctx.cfg = &app.cfg;
    sctx.names = &names;
    std::array<bool, 1000> ops{};
    for (int i = 0; i < 900; ++i) ops[i] = sctx.option(i);
    return ops;
}

}  // namespace

struct SelectPrep {
    std::shared_ptr<Library> lib;
    std::vector<Entry> root;
    std::unordered_map<std::string, int> favs;
    RecordProfile profile;
};

namespace {
std::atomic<const char*> g_prep_step{""};
}

const char* select_prep_step() { return g_prep_step.load(); }

std::shared_ptr<SelectPrep> prepare_select(App& app) {
    struct StepReset {
        ~StepReset() { g_prep_step = ""; }
    } step_reset;
    g_prep_step = "Loading charts";
    auto p = std::make_shared<SelectPrep>();
    SongDb db;  // 自分の接続（裏のスレッドで動かすため、app.db は使わない）
    if (!db.open(song_db_path())) return nullptr;
    p->lib = std::make_shared<Library>();
    Library& lib = *p->lib;
    // 段ごとにかかった時間（0.5 秒を超えた段だけログに書く。起動が遅いときに、どこで待っているかを見る）
    auto lap_t = std::chrono::steady_clock::now();
    auto lap = [&lap_t](const char* what) {
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - lap_t).count();
        if (sec > 0.5) std::printf("選曲画面の一覧づくり：%s %.1f 秒\n", what, sec);
        lap_t = now;
    };
    load_charts(lib, db, app.cfg);
    lap("譜面を読む");
    g_prep_step = "Loading difficulty tables";
    for (const std::string& u : app.cfg.tables) {
        DifficultyTable t;
        if (!load_cached_table(table_cache_dir(), u, t)) t.url = u, t.warning = "まだ取得していない";
        lib.tables.push_back(std::move(t));
    }
    lap("難易度表を読む");
    g_prep_step = "Loading scores";
    p->profile = record_profile(db, app);
    lib.load_records(db, app.cfg.ln_mode, p->profile);
    p->favs = db.favorites();
    lap("記録を読む");
    lib.build_indexes();
    lap("索引");
    g_prep_step = "Building custom folders";
    load_custom(lib, app, p->favs);
    lap("カスタムフォルダと MOD のフォルダ");
    g_prep_step = "Building song list";
    p->root = build_tree(lib, new_song_since(app.cfg.title_flash), p->favs);
    lap("一覧を組む");
    return p;
}

namespace {
bool g_select_skin_preloaded = false;
}

void preload_select_skin(App& app) {
    OptionNames names;
    app.load_select_skin(select_load_options(app, names));
    g_select_skin_preloaded = true;
}

void run_select(App& app, std::shared_ptr<SelectPrep> prep) {
    SDL_Renderer* r = app.renderer;
    TextCache text(r);
    app.set_view(1280, 720);
    if (!prep) prep = prepare_select(app);

    // 手元にあるものですぐに一覧を出し、曲フォルダの読み直しと難易度表の更新は裏で行う
    // 開発用：RD_TIMING があれば、最初の絵までの内訳を出す
    uint64_t lap_ns = SDL_GetTicksNS();
    auto lap = [&](const char* what) {
        if (!dev_env("RD_TIMING")) return;
        uint64_t now = SDL_GetTicksNS();
        std::printf("  選曲画面の準備：%s %.0f ms\n", what, (now - lap_ns) / 1e6);
        lap_ns = now;
    };
    // 下ごしらえ（起動の画面で裏で作ったもの。無ければ上で作った）
    std::shared_ptr<Library> lib = prep ? prep->lib : std::make_shared<Library>();
    // MOD の組み合わせごとに記録を分ける（判定の欄で判定を替えると変わる）。7k モードも別
    RecordProfile profile = prep ? prep->profile : RecordProfile{};
    std::unordered_map<std::string, int> favs = prep ? std::move(prep->favs) : std::unordered_map<std::string, int>{};
    std::vector<Entry> root = prep ? std::move(prep->root) : std::vector<Entry>{};
    prep.reset();
    lap("一覧");
    if (dev_env("RD_MEMSTAT")) {
        // 開発用：一覧のメモリの見積もり（要素の数と、文字列が持つ分）
        size_t entries = 0, charts_e = 0, missing = 0, folders = 0, str_bytes = 0;
        auto sbytes = [](const std::string& x) { return x.capacity() > 15 ? x.capacity() + 1 : 0; };
        std::function<void(const Entry&)> walk = [&](const Entry& e) {
            ++entries;
            if (e.kind == Entry::Kind::Chart) ++charts_e;
            else if (e.kind == Entry::Kind::Missing) ++missing;
            else ++folders;
            str_bytes += sbytes(e.label) + sbytes(e.sub) + sbytes(e.badge) + sbytes(e.note) + sbytes(e.dir);
            str_bytes += sbytes(e.missing().md5) + sbytes(e.missing().sha256) + sbytes(e.missing().level) + sbytes(e.missing().title) +
                         sbytes(e.missing().artist) + sbytes(e.missing().url) + sbytes(e.missing().url_diff);
            for (const Entry& c : e.children) walk(c);
        };
        for (const Entry& e : root) walk(e);
        size_t rec_str = 0;
        for (const ChartRecord& c : lib->charts)
            rec_str += sbytes(c.path) + sbytes(c.folder) + sbytes(c.sha256) + sbytes(c.md5) + sbytes(c.title) + sbytes(c.subtitle) +
                       sbytes(c.artist) + sbytes(c.subartist) + sbytes(c.genre) + sbytes(c.banner) + sbytes(c.stagefile) +
                       sbytes(c.backbmp);
        size_t table_bytes = 0;
        for (const DifficultyTable& t : lib->tables)
            for (const TableChart& c : t.charts)
                table_bytes += sizeof(TableChart) + sbytes(c.md5) + sbytes(c.sha256) + sbytes(c.level) + sbytes(c.title) +
                               sbytes(c.artist) + sbytes(c.url) + sbytes(c.url_diff);
        std::printf("メモリ：譜面 %zu 件 × %zu B ＋ 文字列 %.1f MB ／ 一覧の要素 %zu 件（譜面 %zu・未所持 %zu・フォルダ %zu）× %zu B ＋ 文字列 %.1f MB ／ "
                    "難易度表 %.1f MB ／ 記録 %zu 件\n",
                    lib->charts.size(), sizeof(ChartRecord), rec_str / 1e6, entries, charts_e, missing, folders, sizeof(Entry),
                    str_bytes / 1e6, table_bytes / 1e6, lib->scores.size());
    }
    // フォルダランプ（LR2: folderlamp）。フォルダの中の譜面のランプの一番低いもの。記録か一覧が変わったら数え直す。
    // 鍵は「鍵盤数の絞り込み・たどってきたフォルダの名前・そのフォルダの名前」。一覧の要素の場所（ポインタ）で覚えると、
    // フォルダを出入りして一覧を作り直したときに、別のフォルダが同じ場所に入って、そのランプが出てしまう
    std::unordered_map<std::string, int> folder_lamps;
    // 譜面ごとのランプ（lib->charts の並びの順）。フォルダのランプを数えるときに、sha256 の文字列で記録を
    // 探さずに済むように前もって並べておく（何万もの譜面を見るので、探すたびに文字列を数えると数十 ms かかる）。
    // 記録や一覧が変わったら空にし、次に要ったときに作り直す
    std::vector<const ScoreRecord*> chart_clear;  // 譜面ごとの記録（無ければ nullptr）
    const Library* chart_clear_lib = nullptr;
    // フォルダの記録の合計（スコアの欄。鍵はフォルダランプと同じ）
    struct FolderTotal {
        ScoreRecord sc;
        int notes = 0, played_notes = 0;
        int charts = 0, played = 0;  // 持っている譜面の数・遊んだ譜面の数
    };
    std::unordered_map<std::string, FolderTotal> folder_totals;
    std::string folder_totals_pending;  // 中身を集めている途中で数えた合計の鍵（次のフレームで数え直す）
    std::vector<int> folder_played;  // カーソルのフォルダの「遊んだ譜面の数・持っている数・割合（%）」
    // カスタムフォルダの中身を、裏の 1 本のスレッドで集める（フォルダランプと合計のため）。
    // .lr2folder が多いと、画面の処理の中で 1 つずつ SQL を回すと、カーソルを動かすたびに固まる
    struct CustomPrefetch {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::pair<std::shared_ptr<Library>, const CustomFolderDef*>> jobs;
        std::set<const CustomFolderDef*> queued;
        bool stop = false;
        bool running = false;  // 裏で 1 つ集めている最中
    };
    auto prefetch = std::make_shared<CustomPrefetch>();
    std::thread prefetch_thread([p = prefetch] {
        std::unique_lock<std::mutex> lock(p->mu);
        for (;;) {
            p->cv.wait(lock, [&] { return p->stop || !p->jobs.empty(); });
            if (p->stop) return;
            auto job = std::move(p->jobs.front());
            p->jobs.pop_front();
            p->running = true;
            lock.unlock();
            if (job.first->custom_query) job.first->custom_query->run(*job.second);  // 結果は問い合わせの側が覚える
            lock.lock();
            p->queued.erase(job.second);
            p->running = false;
            p->cv.notify_all();
        }
    });
    // フォルダランプ・合計のために中身が要るカスタムフォルダ。集め終えていれば中身を入れて true、
    // まだなら裏に頼んで false（そのフレームはランプを出さず、次のフレームでまた見る）。
    // 開くたびに 1 曲を選び直すフォルダ（#MAXTRACKS 1）は中身を数えない
    auto custom_ready = [&](const Entry& x) {
        if (!x.custom || x.custom_loaded) return true;
        if (x.custom->max_tracks == 1 || !lib->custom_query) return true;
        std::vector<std::string> md5s;
        if (lib->custom_query->cached(*x.custom, md5s)) {
            fill_custom_entry(*lib, x, md5s);
            return true;
        }
        std::lock_guard<std::mutex> lock(prefetch->mu);
        if (prefetch->queued.insert(x.custom).second) {
            prefetch->jobs.emplace_back(lib, x.custom);
            prefetch->cv.notify_one();
        }
        return false;
    };
    auto folder_lamp_key = [](int mode, const std::string& path, const Entry& e) {
        return std::to_string(mode) + "\x1f" + path + "\x1f" + e.label;
    };
    // 譜面の記録と、lib->charts の中での番号（一覧の外の譜面なら -1）
    auto chart_score = [&](const ChartRecord* c, int* index = nullptr) -> const ScoreRecord* {
        if (chart_clear_lib != lib.get() || chart_clear.size() != lib->charts.size()) {
            chart_clear.assign(lib->charts.size(), nullptr);
            for (size_t i = 0; i < lib->charts.size(); ++i) {
                auto sc = lib->scores.find(lib->charts[i].sha256);
                if (sc != lib->scores.end()) chart_clear[i] = &sc->second;
            }
            lib->build_indexes();
            chart_clear_lib = lib.get();
        }
        const ChartRecord* base = lib->charts.data();
        if (c >= base && c < base + lib->charts.size()) {
            if (index) *index = static_cast<int>(c - base);
            return chart_clear[c - base];
        }
        if (index) *index = -1;
        auto sc = lib->scores.find(c->sha256);  // 一覧の外の譜面（無いはずだが念のため）
        return sc != lib->scores.end() ? &sc->second : nullptr;
    };
    auto chart_lamp = [&](const ChartRecord* c) -> int {
        const ScoreRecord* r = chart_score(c);
        return r ? r->clear : 0;
    };

    std::mutex mu;
    std::shared_ptr<Library> pending;
    // pending の一覧の木（裏で作ってあれば。無ければ表へ移すときに作る）。作るのに数百 ms かかるので、
    // 画面の処理の中で作ると、曲フォルダが増えるたびに選曲画面が止まる
    std::shared_ptr<std::vector<Entry>> pending_root;
    std::atomic<bool> busy{false};
    std::atomic<bool> stop_worker{false};  // 選曲画面を抜けるときに、裏の読み込みを止める
    // 譜面の傾向を使う MOD があれば、まだ分析していない譜面を裏で分析する。終わったら MOD のフォルダを作り直す
    std::atomic<bool> attr_ready{false};
    std::thread attr_worker;
    {
        const auto mods_now = app.active_mods();
        if (std::any_of(mods_now.begin(), mods_now.end(), [](const ModManifest* m) { return m->chart_attr; }))
            attr_worker = std::thread([&, mods_now] {
                SongDb db;
                if (!db.open(song_db_path())) return;
                // 配った分析の結果を先に入れる（その譜面は分析しない）
                for (const ModManifest* m : mods_now)
                    if (m->chart_attr)
                        if (int n = db.import_chart_attrs_if_changed(m->dir + "/chart_attr.tsv", m->id); n > 0) {
                            std::printf("配った譜面の傾向を入れた：%d\n", n);
                            attr_ready = true;
                        }
                const int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2));
                int total = 0;
                while (!stop_worker) {
                    const int n = db.fill_chart_attrs(300, threads);
                    if (n == 0) break;
                    total += n;
                    if (total % 3000 < n) std::printf("譜面の傾向を分析した：%d\n", total);
                }
                if (total > 0 && !stop_worker) std::printf("譜面の傾向の分析を終えた：%d\n", total), attr_ready = true;
            });
    }
    std::string status = "";
    std::thread worker;
    // scan_songs：曲フォルダも読み直すか（false なら難易度表だけ取りに行く）。
    // only_under：そのフォルダの下だけを読み直す（F8。難易度表は取りに行かない）
    auto refresh = [&](bool scan_songs = true, std::string only_under = {}) {
        if (busy) return;
        if (worker.joinable()) worker.join();
        busy = true;
        // 難易度表は取り直さないときは今のものを使う。写すのは裏で（何万もの譜面の表を、画面の処理の中で写すと止まる）
        std::shared_ptr<const Library> keep_from = only_under.empty() ? nullptr : std::shared_ptr<const Library>(lib);
        worker = std::thread([&, scan_songs, only_under, keep_from, dirs = app.cfg.song_dirs,
                              urls = app.cfg.tables, ln_mode = app.cfg.ln_mode, profile, mods = app.active_mods(),
                              custom_dirs = app.cfg.custom_folder_dirs, favs_now = favs, new_since = new_song_since(app.cfg.title_flash),
                              cfg_now = app.cfg] {
            // 一覧の木まで裏で作る（表で作ると数百 ms 止まる）
            auto make_root = [&](Library& l) {
                l.build_indexes();
                load_custom(l, mods, custom_dirs, favs_now);
                return std::make_shared<std::vector<Entry>>(build_tree(l, new_since, favs_now));
            };
            // 曲データベースは別の接続で書く（書き込みはこのスレッドだけ）
            SongDb wdb;
            ScanStats st;
            if (wdb.open(song_db_path()) && scan_songs)
                st = wdb.scan(dirs, static_cast<int>(std::max(2u, std::thread::hardware_concurrency())), nullptr, only_under);
            auto next = std::make_shared<Library>();
            next->roots = dirs;
            next->charts = playable_charts(wdb);
            fill_difficulties(next->charts);
            next->load_records(wdb, ln_mode, profile);
            if (keep_from) next->tables = keep_from->tables;
            else
                for (const std::string& u : urls) next->tables.push_back(update_table(table_cache_dir(), u));
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d 譜面（追加 %d／変更 %d／削除 %d）%.1f 秒", st.found, st.added, st.updated,
                          st.removed, st.seconds);
            auto next_root = make_root(*next);
            {
                std::lock_guard<std::mutex> lock(mu);
                pending = next;
                pending_root = next_root;
                status = buf;
            }
            // 外部ツールのための LR2 の形の曲データベースとスコア（一覧を渡したあとで）
            if (scan_songs) write_lr2_files(wdb, cfg_now, true);
            // LR2 から取り込んだ仮の譜面を、裏で少しずつ読んで本物の情報に置き換える。
            // 読み終えたときに一覧へ反映する
            const int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2));
            int total = wdb.provisional_count(), done = 0;
            auto last = std::chrono::steady_clock::now();
            while (total > 0 && !stop_worker) {
                int n = wdb.fill_provisional(200, threads);
                done += n;
                bool finished = n == 0 || done >= total;
                // 途中では一覧を作り直さない（作り直すと選曲の邪魔になる）。読み終えたときと、10 分ごとに反映する
                if (finished || std::chrono::steady_clock::now() - last > std::chrono::minutes(10)) {
                    auto lib2 = std::make_shared<Library>();
                    lib2->roots = dirs;
                    lib2->charts = playable_charts(wdb);
                    fill_difficulties(lib2->charts);
                    lib2->load_records(wdb, ln_mode, profile);
                    lib2->tables = next->tables;
                    std::snprintf(buf, sizeof(buf), "譜面を読んでいます %d / %d", done, total);
                    auto root2 = make_root(*lib2);
                    std::lock_guard<std::mutex> lock(mu);
                    pending = lib2;
                    pending_root = root2;
                    status = finished ? "譜面を読み終えた" : buf;
                    last = std::chrono::steady_clock::now();
                }
                if (finished) break;
            }
            busy = false;
        });
    };
    // 曲フォルダの監視（RD の拡張）。変わったフォルダを覚え、変化が落ち着いてから 1 つずつ読み直す
    FolderWatcher& watcher = app.watcher;
    std::deque<std::string> watched_dirs;
    // 起動時に曲フォルダと難易度表を読み直す（設定で「手動」なら読み直さない。F8 で今のフォルダだけ）
    // 起動の画面で読み直し済みなら、ここでは読み直さない
    if (app.cfg.db_update == 0 && !app.updated_at_start) refresh();

    // 絞り込みとソートは設定に残し、次に起動しても同じにする（LR2 と同じ）。
    // 難易度の絞り込み（LR2: select.difficulty。0=ALL 1..5=BEGINNER..INSANE）。譜面の並ぶ一覧だけに効く
    int& difficulty_filter = app.cfg.select_difficulty;
    // 鍵盤数の絞り込みとソートは、LR2 ではなく beatoraja の項目と並びに合わせる（RD の拡張。LR2 のソートは実用にならないため）。
    // 鍵盤数：kModeNames の順。RD の譜面は 7 鍵と 5 鍵だけなので、14KEY などでは譜面が出ない（出る項目まで進める）
    int& mode_filter = app.cfg.select_mode;
    // ソート：kSortNames の順。0 の OFF は並べ替えない（難易度表やフォルダの順のまま）
    int& sort_mode = app.cfg.select_sort;
    struct Level {
        const std::vector<Entry>* full;  // 絞り込む前の一覧
        int cursor;
        std::string name;
        std::shared_ptr<std::vector<Entry>> view;  // 絞り込んだ一覧（絞り込んでいなければ空）
        const std::vector<Entry>* list;  // 見せている一覧
        bool lr2_filter = false;         // 曲フォルダの一覧（LR2 の難易度の絞り込みをかける。難易度表などは常に ALL）
        std::string dir;                 // 曲フォルダの一覧なら、そのフォルダのパス
        int bar_type = 1;                // このフォルダのバーの種類（フォルダの名前のバーに使う）
        int head = 0;                    // 一覧の頭に足したバー（フォルダの名前・MOD のバー）の数
        bool keep_order = false;         // 並べ替えをかけない（MOD が並べた順のまま）
    };
    // MOD がフォルダに足すバー（フォルダの名前と譜面の数ごとに、1 回だけ聞く）
    std::map<std::pair<std::string, int>, std::vector<ModBar>> mod_bar_cache;
    auto mod_bars_for = [&](const std::string& folder, int charts) -> const std::vector<ModBar>& {
        auto [it, fresh] = mod_bar_cache.try_emplace({folder, charts});
        if (fresh) {
            std::vector<std::string> errors;
            it->second = mod_folder_bars(app.active_mods(), folder, charts, mod_data_dir(""), &errors);
            for (const std::string& er : errors) std::printf("MOD のバーを作れない：%s\n", er.c_str());
        }
        return it->second;
    };
    auto make_level = [&](const std::vector<Entry>* full, int cursor, const std::string& name, bool lr2_filter = false,
                          std::string dir = {}, int bar_type = 1, bool keep_order = false) {
        Level lv{full, cursor, name, nullptr, full, lr2_filter, std::move(dir), bar_type};
        lv.keep_order = keep_order;
        // 鍵盤数の絞り込みで譜面が 1 つも出なくなるなら、出る項目まで進める（LR2: songmanage:2590）
        auto keys_of = [](const Entry& e) { return e.kind == Entry::Kind::Chart && e.chart ? e.chart->keys : 7; };
        bool any_song = false;
        for (const Entry& e : *full) any_song = any_song || e.kind == Entry::Kind::Chart || e.kind == Entry::Kind::Missing;
        for (int tries = 0; any_song && tries < kModeCount; ++tries) {
            bool shown = false;
            for (const Entry& e : *full)
                if ((e.kind == Entry::Kind::Chart || e.kind == Entry::Kind::Missing) && mode_shows(mode_filter, keys_of(e))) shown = true;
            if (shown) break;
            mode_filter = (mode_filter + 1) % kModeCount;
        }
        // 難易度の絞り込み（LR2: songmanage:2034）。曲フォルダ（同じフォルダの差分）ごとに、選んだ難易度以下で一番上の譜面を出す。
        // 以下が無ければ一番下の譜面。曲は消えない。選んだ難易度の譜面が一覧に 1 つも無ければ、難易度を自動で進める
        bool use_diff = lr2_filter && difficulty_filter != 0;
        std::unordered_map<std::string, int> shown_diff;  // 曲フォルダ → 出す難易度
        if (use_diff) {
            for (int tries = 0; tries < 6 && difficulty_filter != 0; ++tries) {
                bool exact = false;
                for (const Entry& e : *full)
                    if (e.kind == Entry::Kind::Chart && e.chart->difficulty == difficulty_filter) exact = true;
                if (exact) break;
                difficulty_filter = (difficulty_filter + 1) % 6;
            }
            use_diff = difficulty_filter != 0;
            std::unordered_map<std::string, std::pair<int, int>> range;  // 曲フォルダ → (d 以下で一番上, 一番下)
            for (const Entry& e : *full) {
                if (e.kind != Entry::Kind::Chart) continue;
                auto [it, fresh] = range.try_emplace(e.chart->folder, std::make_pair(-1, 99));
                int d = e.chart->difficulty;
                if (d <= difficulty_filter) it->second.first = std::max(it->second.first, d);
                it->second.second = std::min(it->second.second, d);
            }
            for (auto& [f, r] : range) shown_diff[f] = r.first >= 0 ? r.first : r.second;
        }
        const int sort = keep_order ? 0 : sort_mode;
        if (use_diff || mode_filter != 0 || sort != 0) {
            auto v = std::make_shared<std::vector<Entry>>();
            for (const Entry& e : *full) {
                if (e.kind != Entry::Kind::Folder && e.kind != Entry::Kind::Course) {
                    if (!mode_shows(mode_filter, keys_of(e))) continue;
                    if (use_diff && e.kind == Entry::Kind::Chart && e.chart->difficulty != shown_diff[e.chart->folder]) continue;
                }
                v->push_back(e);
            }
            // 譜面だけを並べ替える（フォルダは元の順のまま前、持っていない譜面は後ろ）。
            // 記録で並べるもの（CLEAR / SCORE / MISSCOUNT）は、記録の無い譜面を後ろに回す。同じならタイトル順
            if (sort != 0) {
                // 文字の並びは LR2 と同じく、Shift_JIS にして英字を大文字にしたもののバイト順
                std::unordered_map<std::string, std::string> memo;
                auto key = [&](const std::string& s) -> const std::string& {
                    auto [it, fresh] = memo.try_emplace(s);
                    if (fresh) {
                        std::string k = utf8_to_cp932(s);
                        for (size_t i = 0; i < k.size(); ++i) {
                            unsigned char ch = static_cast<unsigned char>(k[i]);
                            if ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC)) {
                                ++i;
                                continue;
                            }
                            if (ch >= 'a' && ch <= 'z') k[i] = static_cast<char>(ch - 32);
                        }
                        it->second = std::move(k);
                    }
                    return it->second;
                };
                auto less_str = [&](const std::string& x, const std::string& y) {
                    const std::string& kx = key(x);
                    const std::string& ky = key(y);
                    return std::lexicographical_compare(kx.begin(), kx.end(), ky.begin(), ky.end(), [](char p, char q) {
                        return static_cast<unsigned char>(p) < static_cast<unsigned char>(q);
                    });
                };
                auto title_of = [](const Entry& e) -> const std::string& {
                    return e.kind == Entry::Kind::Chart ? e.chart->title : e.kind == Entry::Kind::Missing ? e.missing().title : e.label;
                };
                auto artist_of = [](const Entry& e) -> const std::string& {
                    return e.kind == Entry::Kind::Chart ? e.chart->artist : e.kind == Entry::Kind::Missing ? e.missing().artist : e.sub;
                };
                // 譜面だけを並べ替える（フォルダは元の順のまま前）。曲名・アーティストは持っていない譜面も一緒に並べる。
                // それ以外（BPM・長さ・レベル・記録）は持っていない譜面を後ろに回し、記録で並べるものは記録の無い譜面も後ろ。同じなら曲名順
                std::stable_sort(v->begin(), v->end(), [&](const Entry& a, const Entry& b) {
                    bool fa = a.kind == Entry::Kind::Folder || a.kind == Entry::Kind::Course,
                         fb = b.kind == Entry::Kind::Folder || b.kind == Entry::Kind::Course;
                    if (fa || fb) return fa && !fb;
                    auto by_title = [&] {
                        const std::string& ta = title_of(a);
                        const std::string& tb = title_of(b);
                        if (ta != tb) return less_str(ta, tb);
                        int da = a.kind == Entry::Kind::Chart ? a.chart->difficulty : 0, db = b.kind == Entry::Kind::Chart ? b.chart->difficulty : 0;
                        return da < db;
                    };
                    if (sort_mode == 1) return by_title();
                    if (sort_mode == 2) {
                        const std::string& xa = artist_of(a);
                        const std::string& xb = artist_of(b);
                        return xa != xb ? less_str(xa, xb) : by_title();
                    }
                    const ChartRecord* ca = a.kind == Entry::Kind::Chart ? a.chart : nullptr;
                    const ChartRecord* cb = b.kind == Entry::Kind::Chart ? b.chart : nullptr;
                    if (!ca || !cb) return ca ? true : cb ? false : by_title();
                    const ScoreRecord* sa = nullptr;
                    const ScoreRecord* sb = nullptr;
                    if (auto it = lib->scores.find(ca->sha256); it != lib->scores.end() && it->second.playcount > 0) sa = &it->second;
                    if (auto it = lib->scores.find(cb->sha256); it != lib->scores.end() && it->second.playcount > 0) sb = &it->second;
                    auto by_record = [&](auto k) {
                        if (!sa || !sb) return sa != nullptr && sb == nullptr ? true : (!sa && !sb ? by_title() : false);
                        auto ka = k(*sa, *ca), kb = k(*sb, *cb);
                        return ka != kb ? ka < kb : by_title();
                    };
                    switch (sort_mode) {
                    case 3: return ca->bpm_max != cb->bpm_max ? ca->bpm_max < cb->bpm_max : by_title();
                    case 4: return ca->length_ms != cb->length_ms ? ca->length_ms < cb->length_ms : by_title();
                    case 5:
                        if (ca->playlevel != cb->playlevel) return ca->playlevel < cb->playlevel;
                        return ca->difficulty != cb->difficulty ? ca->difficulty < cb->difficulty : by_title();
                    case 6: return by_record([](const ScoreRecord& s, const ChartRecord&) { return static_cast<double>(s.clear); });
                    case 7:
                        return by_record([&](const ScoreRecord& s, const ChartRecord& c) {
                            int n = c.notes + (app.cfg.ln_mode > 0 ? c.ln_free : 0);
                            return n > 0 ? static_cast<double>(s.exscore) / (n * 2) : 0.0;
                        });
                    default: return by_record([](const ScoreRecord& s, const ChartRecord&) { return static_cast<double>(s.bp); });
                    }
                });
            }
            lv.view = v;
            lv.list = v.get();
        }
        // フォルダの中では、頭にフォルダの名前のバーを置き、その下に MOD のバーを置く。
        // 一覧は輪なので、一番下の項目の次にフォルダの名前が来て、どこが端か分かる。開いたときのカーソルはフォルダの名前
        if (full != &root) {
            auto v = lv.view ? lv.view : std::make_shared<std::vector<Entry>>(*full);
            int charts = 0;
            for (const Entry& e : *v) charts += e.kind == Entry::Kind::Chart;
            std::vector<Entry> head(1);
            head[0].kind = Entry::Kind::Header;
            head[0].label = name;
            head[0].bar_type = bar_type;
            for (const ModBar& b : mod_bars_for(name, charts)) {
                Entry e;
                e.kind = Entry::Kind::ModBar;
                e.label = b.title;
                e.bar_type = 0;  // 曲と同じ色
                e.mod_info = std::make_shared<const ModBar>(b);
                head.push_back(std::move(e));
            }
            lv.head = static_cast<int>(head.size());
            v->insert(v->begin(), std::make_move_iterator(head.begin()), std::make_move_iterator(head.end()));
            lv.view = v;
            lv.list = v.get();
        }
        // メモリを節約しない設定では、前と同じく、一覧の写しにフォルダの中身を下の階層まで丸ごと写して持たせる
        if (lv.view && !app.cfg.low_memory) {
            std::function<void(Entry&)> deep = [&](Entry& e) {
                if (e.children.empty()) return;
                e.children.detach();
                for (Entry& c : e.children.vec()) deep(c);
            };
            for (Entry& e : *lv.view) deep(e);
        }
        return lv;
    };
    std::vector<Level> stack{make_level(&root, 0, "RD")};
    // いまの階層までのフォルダの名前（folder_lamp_key に使う）
    auto level_path = [&] {
        std::string p;
        for (const Level& l : stack) p += l.name + "\x1f";
        return p;
    };
    // フォルダ → その中の譜面（一覧を差し替えたら作り直す）
    // 選曲 BGM（ループ）は、最初の絵を出してから鳴らす（準備のあいだに音だけが先に鳴らないように）
    bool bgm_pending = true;
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
    bool back_from_play = false;  // このフレームで遊んで戻ってきた（遊んでいた時間を、遅いフレームとして数えない）
    int prev_hold_dir = 0;
    bool quit = false;

    // 選曲スキン（docs/lr2_select_spec.md）。読めなければ RD の仮の画面
    OptionNames opt_names;
    lap("そのほか");
    std::array<bool, 1000> load_ops = select_load_options(app, opt_names);
    SelectContext sctx;
    sctx.cfg = &app.cfg;
    sctx.names = &opt_names;
    // 起動の画面で読んでおいたら、もう一度は読まない
    if (!g_select_skin_preloaded) app.load_select_skin(load_ops);
    g_select_skin_preloaded = false;
    Lr2Skin* sk = app.select_skin.get();
    sctx.skin = sk;
    lap("スキン");
    // スキンセレクト（WMIX のパネル2・LR2 標準のスキンセレクト画面）
    SkinCatalog catalog;
    catalog.scan(app.lr2_root);
    sctx.catalog = &catalog;
    sctx.player = app.db.player_stats();
    {
        // プレイヤーの名前（LR2 から引き継いだ名前、無ければ ID）
        const auto info = app.db.lr2_player_info();
        auto it = info.find("name");
        if (it == info.end() || it->second.empty()) it = info.find("id");
        sctx.player_name = it != info.end() ? it->second : player_id();  // 引き継いでいなければ RD のプレイヤーの ID
    }
    uint64_t scene_start = SDL_GetTicksNS();
    auto scene_ms = [&] { return (SDL_GetTicksNS() - scene_start) / 1e6; };
    sctx.timers.set(0, 0);
    // 曲リストのスクロール（仕様 3.1・3.5。1曲=1000。カーソルの曲は lv.cursor が持ち、見た目だけを補間する）
    int64_t list_top = 0, old_bar = 0, now_bar = 0;
    double move_start = 0, move_end = 0;
    float mouse_x = -1, mouse_y = -1;
    double mouse_moved_ms = -1e9;
    // 描く大きさを選曲スキンに合わせる。restart なら画面を始め直す（LR2 と同じく、プレイ・決定画面・キーコンフィグ・
    // スキンセレクトから戻るたびに、タイマー 0 から入場の演出をやり直し、#STARTINPUT まで入力を受けない）
    auto set_select_view = [&](bool restart = true) {
        if (sk) app.set_view(sk->width, sk->height);
        else app.set_view(1280, 720);
        if (!restart) return;
        scene_start = SDL_GetTicksNS();
        sctx.timers = SkinTimers();
        sctx.timers.set(0, 0);
        move_start = move_end = 0;
        list_top = old_bar = now_bar;
        mouse_moved_ms = -1e9;
    };
    set_select_view(false);
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
    // 保存したランダムの配置の一覧（F9。RD。仕様書6章「ランダム配置の保存」）。0 行目は「使わない」、1 行目から新しい順
    bool layout_open = false;
    std::vector<SavedRandom> layouts;
    std::string layout_sha;
    int layout_cursor = 0;
    auto open_layouts = [&](const std::string& sha256) {
        layout_sha = sha256;
        layouts = app.db.saved_randoms(sha256);
        layout_cursor = 0;
        for (size_t i = 0; i < layouts.size(); ++i)
            if (layouts[i].used) layout_cursor = static_cast<int>(i) + 1;
        layout_open = true;
        app.mixer.play_system(kSysOptionChange);
    };
    auto close_layouts = [&] {
        if (!layout_open) return;
        layout_open = false;
        app.mixer.play_system(kSysOptionChange);
    };
    auto step_layout = [&](int d) {
        const int n = static_cast<int>(layouts.size()) + 1;
        layout_cursor = ((layout_cursor + d) % n + n) % n;
    };
    // カーソルの配置を、この譜面で使う配置にする（「使わない」なら外す）
    auto choose_layout = [&] {
        app.db.use_saved_random(layout_sha, layout_cursor > 0 ? layouts[layout_cursor - 1].id : 0);
        close_layouts();
    };
    auto delete_layout = [&] {
        if (layout_cursor <= 0) return;
        app.db.delete_saved_random(layouts[layout_cursor - 1].id);
        layouts.erase(layouts.begin() + (layout_cursor - 1));
        layout_cursor = std::min(layout_cursor, static_cast<int>(layouts.size()));
        app.mixer.play_system(kSysOptionChange);
    };
    // バナー（仕様 2.17）。カーソルの曲が変わるたびに裏で読み、読めたら 102 番の画像にする。
    // OpenLR2 は #RELOADBANNER のスキンだけで読むが、LR2 では指定の無い WMIX でも出るので、いつも読む
    struct BannerLoad {
        std::mutex mu;
        std::condition_variable cv;
        std::string want;          // いま読んでほしいもの（画面の処理が書く。待たずに次々書き換えてよい）
        std::string want_folder;   // 付属テキストの有無を確かめてほしいフォルダ
        std::string path;          // 読み終えたもの
        SkinImage image;
        bool ready = false;
        std::unordered_map<std::string, bool> folder_text;  // フォルダ → 付属テキスト（.txt）があるか
        bool stop = false;
    };
    auto banner = std::make_shared<BannerLoad>();
    // バナーと付属テキストの有無は、裏の 1 本のスレッドで読む（曲は HDD にあることが多く、画面の処理の中で読むと
    // 冷えた HDD ではカーソルを動かすたび・フォルダを開くたびに待たされる）。いつも一番新しく頼まれたものだけを読む
    std::thread banner_thread([b = banner] {
        std::unique_lock<std::mutex> lock(b->mu);
        for (;;) {
            b->cv.wait(lock, [&] { return b->stop || !b->want.empty() || !b->want_folder.empty(); });
            if (b->stop) return;
            std::string want = std::move(b->want), folder = std::move(b->want_folder);
            b->want.clear(), b->want_folder.clear();
            lock.unlock();
            int text = -1;
            if (!folder.empty()) {
                text = 0;
                std::error_code ec;
                for (const auto& de : fs::directory_iterator(folder, ec)) {
                    std::string ext = de.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (ext == ".txt") text = 1;
                }
            }
            SkinImage img;
            if (!want.empty()) {
                // 書かれた名前のファイルが無ければ、拡張子だけ違うものを探す（よくある）
                fs::path p(want);
                std::error_code ec;
                if (!fs::exists(p, ec))
                    for (const char* ext : {".png", ".jpg", ".bmp", ".PNG", ".JPG", ".BMP"}) {
                        fs::path q = p;
                        q.replace_extension(ext);
                        if (fs::exists(q, ec)) {
                            p = q;
                            break;
                        }
                    }
                static const uint8_t black[3] = {0, 0, 0};
                if (fs::exists(p, ec)) load_skin_image(p.string(), img, black, false);
            }
            lock.lock();
            if (text >= 0) b->folder_text[folder] = text == 1;
            if (!want.empty()) b->path = want, b->image = std::move(img), b->ready = true;
        }
    });
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
            // 止めるだけにして、待たない（カーソルを動かすたびに音の側の処理 2 回ぶん待つと、スクロールがカクつく）。
            // 音の側が最後に読み終わるまで捨てられないので、裏で少し置いてから捨てる（キー音の大きなメモリを返す手間も裏で）
            app.mixer.stop();
            std::thread([p = std::move(preview)]() mutable {
                SDL_Delay(300);
                p.reset();
            }).detach();
        }
        preview_path.clear();
        if (restore_bgm) app.mixer.fade_system_loop(1.0f, 1000);
    };
    SDL_Texture* banner_tex = nullptr;
    // バナーの場所（文字列だけ。ファイルがあるかは裏のスレッドが確かめる。ここで HDD を見ると、毎フレーム待たされる）
    auto banner_path_of = [](const ChartRecord& c) -> std::string {
        if (c.banner.empty()) return {};
        return (fs::path(c.folder) / c.banner).string();
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
    // 一覧を作り直す（曲の読み直しや記録の更新のあと）。カーソルは番号ではなく、同じ曲（譜面ファイル・コース・フォルダ）に残す。
    // カーソルの曲が同じ位置のままなら、スクロールの動きも止めない
    auto entry_key = [](const Entry& e) -> std::string {
        if (e.kind == Entry::Kind::Chart) return "c:" + e.chart->path;
        if (e.kind == Entry::Kind::Course) return "k:" + e.course->hash;
        if (e.kind == Entry::Kind::Missing) return "m:" + e.missing().md5 + e.missing().sha256;
        if (e.kind == Entry::Kind::Header) return std::string("h:");
        if (e.kind == Entry::Kind::ModBar) return "x:" + e.mod_bar().mod_id + "/" + e.mod_bar().key;
        return "f:" + e.label;
    };
    // prebuilt：裏で作ってある一覧の木（あればそれを使い、ここでは作らない）
    auto rebuild = [&](std::shared_ptr<std::vector<Entry>> prebuilt = nullptr) {
        std::vector<std::string> path, keys;
        std::vector<int> cursors;
        for (size_t i = 1; i < stack.size(); ++i) path.push_back(stack[i].name);
        for (const Level& lv : stack) {
            cursors.push_back(lv.cursor);
            int n = static_cast<int>(lv.list->size());
            keys.push_back(n ? entry_key((*lv.list)[std::clamp(lv.cursor, 0, n - 1)]) : "");
        }
        const int old_cursor = stack.back().cursor;
        if (prebuilt) root = std::move(*prebuilt);
        else root = build_tree(*lib, new_song_since(app.cfg.title_flash), favs);
        folder_lamps.clear(), folder_totals.clear(), chart_clear.clear();
        auto place = [&](Level& lv, size_t i) {
            const std::vector<Entry>& list = *lv.list;
            for (size_t k = 0; k < list.size(); ++k)
                if (entry_key(list[k]) == keys[i]) {
                    lv.cursor = static_cast<int>(k);
                    return;
                }
            int n = static_cast<int>(list.size());
            lv.cursor = n ? std::clamp(cursors[i], 0, n - 1) : 0;
        };
        stack = {make_level(&root, cursors[0], "RD")};
        place(stack.back(), 0);
        for (size_t i = 0; i < path.size(); ++i) {
            const std::vector<Entry>& list = *stack.back().list;
            auto it = std::find_if(list.begin(), list.end(),
                                   [&](const Entry& e) { return e.kind == Entry::Kind::Folder && e.label == path[i]; });
            if (it == list.end()) break;
            if (it->custom && !it->custom_loaded) load_custom_entry(*lib, *it);
            stack.push_back(make_level(it->children.ptr(), cursors[i + 1], path[i], it->fs_folder, it->dir,
                                       it->course_folder ? 6 : it->bar_type, it->keep_order));
            place(stack.back(), i + 1);
        }
        if (stack.size() == path.size() + 1) {
            // 同じ階層にいる：カーソルが動いた分だけスクロールの位置もずらして、動きはそのまま続ける
            int64_t d = static_cast<int64_t>(stack.back().cursor - old_cursor) * 1000;
            list_top += d, old_bar += d, now_bar += d;
        } else {
            snap();
        }
    };
    double roulette_ms = 0;
    std::string roulette_title, roulette_sha;  // ルーレットに今出ている譜面
    std::vector<std::pair<std::string, std::string>> roulette_pool;  // ルーレットに出す曲名と sha256
    std::mt19937 roulette_rng{std::random_device{}()};
    bool decide_chart_pending = false;  // MOD のバーが選んだ譜面へカーソルを移した。次のフレームで決定する
    bool start_replay = false;  // 次の決定をリプレイの再生にする（ボタン 19、6 番の鍵盤の長押し）
    bool to_keyconfig = false;    // キーコンフィグへ移るためにフェードアウトしている
    bool to_skin_select = false;  // スキンセレクトへ移るためにフェードアウトしている
    std::string replay_checked;  // リプレイの有無を見た譜面（遊んだあとは空にして見直す）
    bool replay_found = false;
    bool resort_pending = false;  // 記録で並べているときに、記録が変わったので並べ直す（次のフレームで）
    // 遊んだあとに、スコアとプレイヤーの累計を読み直す（refreshed：仮の譜面を読み直したので一覧から作り直す）。
    // played は遊んだ譜面（コースならコースとステージ）の sha256。空なら全部の記録を読み直す
    // 記録（遊んだ・判定を替えて MOD の組み合わせが変わった）を、裏で読み直して一覧を作り直す。カスタムフォルダの中身も
    // 裏で集め終えてから差し替える（表で作ると 0.7 秒ほど止まり、フォルダのランプも少しずつしか出ない）。
    // ほかの読み込みが裏で動いていれば、終わってから始める（records_reload_wanted）
    bool records_reload_wanted = false;
    auto reload_records_async = [&] {
        if (busy) {
            records_reload_wanted = true;
            return;
        }
        records_reload_wanted = false;
        if (worker.joinable()) worker.join();
        busy = true;
        std::shared_ptr<const Library> base = lib;
        worker = std::thread([&, base, ln_mode = app.cfg.ln_mode, profile_now = profile, mods = app.active_mods(),
                              custom_dirs = app.cfg.custom_folder_dirs, favs_now = favs, new_since = new_song_since(app.cfg.title_flash),
                              cfg_now = app.cfg] {
            const uint64_t t0 = SDL_GetTicksNS();
            auto next = std::make_shared<Library>();
            next->roots = base->roots, next->charts = base->charts, next->tables = base->tables, next->attrs = base->attrs;
            const uint64_t t1 = SDL_GetTicksNS();
            SongDb rdb;
            const bool rdb_ok = rdb.open(song_db_path());
            if (rdb_ok) next->load_records(rdb, ln_mode, profile_now);
            next->build_indexes();
            const uint64_t t2 = SDL_GetTicksNS();
            load_custom(*next, mods, custom_dirs, favs_now);
            const uint64_t t3 = SDL_GetTicksNS();
            // カスタムフォルダの中身を集める（問い合わせの結果は表が覚える）のと、一覧の木を作るのは、同時に進める
            uint64_t t_query = 0;
            std::thread queries([&next, &t_query] {
                const uint64_t q0 = SDL_GetTicksNS();
                if (next->custom_query)
                    for (const CustomFolderNode& n : next->custom_nodes)
                        for (const CustomFolderDef& d : n.defs)
                            if (d.max_tracks != 1) next->custom_query->run(d);
                t_query = SDL_GetTicksNS() - q0;
            });
            auto next_root = std::make_shared<std::vector<Entry>>(build_tree(*next, new_since, favs_now));
            const uint64_t t_tree = SDL_GetTicksNS() - t3;
            queries.join();
            auto ms = [](uint64_t a, uint64_t b) { return (b - a) / 1e6; };
            std::printf("記録を裏で読み直した：%.0f ms（写す %.0f、記録 %.0f、カスタムフォルダの表 %.0f、中身 %.0f と一覧 %.0f を同時に）\n",
                        ms(t0, SDL_GetTicksNS()), ms(t0, t1), ms(t1, t2), ms(t2, t3), t_query / 1e6, t_tree / 1e6);
            {
                std::lock_guard<std::mutex> lock(mu);
                pending = next;
                pending_root = next_root;
                busy = false;
            }
            // 外部ツールのための LR2 の形のスコア（一覧を渡したあとで）
            if (rdb_ok) write_lr2_files(rdb, cfg_now, false);
        });
    };
    auto reload_after_play = [&](bool refreshed, const std::vector<std::string>& played = {}) {
        // 表の lib の記録とカスタムフォルダを作り直すので、裏で集めている分は止め、集めている最中の 1 つが終わるのを待つ
        // （起動の直後に判定の欄を替えると、裏が使っている問い合わせを作り直して落ちていた）
        if (!refreshed) {
            std::unique_lock<std::mutex> lock(prefetch->mu);
            prefetch->jobs.clear();
            prefetch->queued.clear();
            prefetch->cv.wait(lock, [&] { return !prefetch->running; });
        }
        const uint64_t t0 = SDL_GetTicksNS();
        auto ms = [](uint64_t a, uint64_t b) { return (b - a) / 1e6; };
        if (refreshed) {
            auto lib2 = std::make_shared<Library>();
            load_charts(*lib2, app.db, app.cfg);
            lib2->tables = lib->tables;
            lib2->load_records(app.db, app.cfg.ln_mode, profile);
            std::lock_guard<std::mutex> lock(mu);
            pending = lib2;
            pending_root.reset();
        } else {
            if (played.empty()) {
                lib->load_records(app.db, app.cfg.ln_mode, profile);
            } else {
                // 遊んだ譜面の記録だけを読み直す（全部を読み直すと、譜面が多いときに選曲画面へ戻るのが遅い）
                for (const std::string& sha : played) {
                    ScoreRecord r;
                    const int id = lib->record_profile_of(sha);
                    if (app.db.get_score(sha, r, app.cfg.ln_mode, id)) lib->scores[sha] = r;
                    lib->custom_lamps[sha] = app.db.custom_lamps(sha, app.cfg.ln_mode, id);
                }
            }
            const uint64_t t1 = SDL_GetTicksNS();
            folder_lamps.clear(), folder_totals.clear(), chart_clear.clear();
            // 記録で集めるフォルダ（カスタムフォルダ・MOD のフォルダ）があるときだけ一覧を作り直す。
            // 無ければ、記録で並べているときに並べ直すだけ
            bool record_folders = !lib->custom_nodes.empty();
            for (const ModManifest* m : app.active_mods())
                if (!m->script.empty() && (m->touches & static_cast<uint32_t>(ModTouch::Control))) record_folders = true;
            if (record_folders && !played.empty()) {
                // 遊んだ譜面の記録はもう入れた。カスタムフォルダと MOD のフォルダは裏で作り直して差し替える
                //（表で作ると 0.7 秒ほど止まる）
                reload_records_async();
                if (sort_mode >= 6) resort_pending = true;
                std::printf("選曲画面に戻る：記録 %.0f ms（フォルダは裏で作り直す）\n", ms(t0, t1));
                sctx.player = app.db.player_stats();
                return;
            }
            if (record_folders && played.empty()) {
                load_custom(*lib, app, favs);
            } else if (record_folders) {
                // カスタムフォルダの表は、遊んだ譜面の行だけ書き換える。MOD のフォルダは作り直す
                if (lib->custom_query)
                    for (const std::string& sha : played) {
                        auto sc = lib->scores.find(sha);
                        if (sc == lib->scores.end()) continue;
                        for (const ChartRecord& c : lib->charts)
                            if (c.sha256 == sha) {
                                lib->custom_query->update_score(c, sc->second);
                                break;
                            }
                    }
                std::vector<std::string> errors;
                lib->mod_folders = make_mod_folders(*lib, app.active_mods(), &errors);
                for (const std::string& er : errors) std::printf("MOD のフォルダを作れない：%s\n", er.c_str());
            }
            const uint64_t t2 = SDL_GetTicksNS();
            if (record_folders) rebuild();
            else if (sort_mode >= 6) resort_pending = true;
            std::printf("選曲画面に戻る：記録 %.0f ms、カスタムフォルダ %.0f ms、一覧 %.0f ms\n", ms(t0, t1), ms(t1, t2),
                        ms(t2, SDL_GetTicksNS()));
        }
        sctx.player = app.db.player_stats();
    };
    // メトロノームの欄（ボタン 902）：メトロノームの MOD を使う・使わないを切り替える（MOD が入っていなければ何もしない）。
    // 記録を残すかどうかの表示（SCORE SAVE）も、これで変わる
    auto toggle_metronome = [&] {
        for (const ModManifest& m : app.mods) {
            if (!m.metronome) continue;
            auto& en = app.cfg.enabled_mods;
            auto it = std::find(en.begin(), en.end(), m.id);
            if (it != en.end()) en.erase(it);
            else en.push_back(m.id);
            app.save_config();
            app.mixer.play_system(kSysOptionChange);
            return;
        }
    };
    // 判定の欄（ボタン 900）：OFF → VERY HARD … EASY → JUDGE1 … → OFF（dir が負なら逆）。MOD の設定に書き、記録をその判定のものに読み直す
    auto cycle_judge = [&](int dir) {
        for (ModManifest& m : app.mods) {
            if (!m.has_setting("judges") ||
                std::find(app.cfg.enabled_mods.begin(), app.cfg.enabled_mods.end(), m.id) == app.cfg.enabled_mods.end())
                continue;
            // 並び：OFF・本体の判定（VERY HARD … EASY）・一覧の判定（JUDGE1〜9）
            const int ns = static_cast<int>(std::size(kStandardJudgeIds));
            const int n = 1 + ns + static_cast<int>(std::min<size_t>(m.judges.size(), 9));
            const int cur = ((judge_choice(m) + dir) % n + n) % n;
            m.judge_use = cur == 0 ? std::string() : cur <= ns ? std::string(kStandardJudgeIds[cur - 1]) : m.judges[cur - 1 - ns].id;
            save_mod_settings(m, mod_settings_path(m.id));
            profile = record_profile(app.db, app);
            reload_records_async();
            app.mixer.play_system(kSysOptionChange);
            return;
        }
    };
    // 譜面を借りる（仕様書10章「借りて返す」）。配信先の一覧（md5・sha256）は、選曲画面に入るたびに裏で取り直す
    std::unordered_set<std::string> borrowable;
    std::future<std::unordered_set<std::string>> borrowable_job;
    const BorrowSource borrow_src{kShareReady ? app.cfg.borrow_url : std::string(), app.cfg.borrow_key};  // 作りかけの間は借りない
    if (borrow_src.ok())
        borrowable_job = std::async(std::launch::async, [borrow_src] {
            std::unordered_set<std::string> out;
            std::vector<std::pair<std::string, std::string>> list;
            std::string err;
            if (borrow_list(borrow_src, list, err)) {
                for (const auto& [md5, sha] : list) out.insert(md5), out.insert(sha);
                std::printf("借りられる譜面：%zu\n", list.size());
            } else {
                std::printf("借りる先につながらない：%s\n", err.c_str());
            }
            return out;
        });
    ChartRecord borrowed_rec;  // 借りた譜面（遊んでいる間だけ）
    Entry borrowed_entry;
    BorrowFolder borrowed_folder;
    bool borrowing = false, kept = false;
    // もらう：借りた曲のフォルダを丸ごと、1 つ目の曲フォルダの下の Received に入れて読み直す（ほかの譜面も取る）
    auto keep_borrowed = [&](std::string& msg) {
        if (app.cfg.song_dirs.empty()) return msg = "曲フォルダが無い", false;
        std::string name = borrowed_folder.name;
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) name = borrowed_folder.id;
        const fs::path dest = fs::u8path(app.cfg.song_dirs[0]) / "Received" / fs::u8path(name);
        std::error_code ec;
        fs::create_directories(dest, ec);
        // 借りた分は置き場から写し、残り（ほかの譜面など）を取る
        fs::copy(fs::u8path(data_dir() + "Borrowed/" + borrowed_folder.id), dest,
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        fs::remove(dest / ".rd-used", ec);
        std::string err;
        if (!borrow_fetch(borrow_src, borrowed_folder, dest.string(), true, nullptr, err)) return msg = "もらえない：" + err, false;
        fs::remove(dest / ".rd-used", ec);
        const int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
        ScanStats s = app.db.scan(app.cfg.song_dirs, threads, nullptr, dest.string());
        std::printf("もらった：%s（%d 譜面）\n", dest.string().c_str(), s.added + s.updated);
        kept = true;
        msg = "もらいました";
        return true;
    };
    // 借りる：譜面と、使う音・画像・動画を置き場に取る。取っている間は進み具合を出し、Esc でやめる
    auto run_borrow = [&](const std::string& hash, std::string& chart_path) {
        struct Job {
            std::atomic<int64_t> done{0}, total{0};
            std::atomic<bool> cancel{false}, finished{false}, ok{false};
            std::string error, chart, dir;
            BorrowFolder folder;
        };
        auto job = std::make_shared<Job>();
        const std::string root = data_dir() + "Borrowed";
        const int64_t limit = static_cast<int64_t>(std::max(1, app.cfg.borrow_cache_mb)) * 1024 * 1024;
        std::thread th([job, hash, root, limit, src = borrow_src] {
            if (borrow_info(src, hash, job->folder, job->error)) {
                job->dir = root + "/" + job->folder.id;
                int64_t need = 0;
                for (const auto& [size, rel] : job->folder.files) need += size;
                borrow_trim(root, std::max<int64_t>(0, limit - need), job->dir);  // 先に場所を空ける
                job->ok = borrow_fetch(src, job->folder, job->dir, false,
                                       [&](int64_t d, int64_t t) {
                                           job->done = d, job->total = t;
                                           return !job->cancel.load();
                                       },
                                       job->error);
                if (job->ok) job->chart = job->dir + "/" + job->folder.chart;
            }
            job->finished = true;
        });
        const float vw = sk ? static_cast<float>(sk->width) : 1280.0f, vh = sk ? static_cast<float>(sk->height) : 720.0f;
        const float k = vh / 720.0f;
        while (!job->finished) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) job->cancel = true, quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) job->cancel = true;
            }
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            const SDL_Color white{235, 235, 240, 255}, dim{140, 140, 150, 255};
            char line[96];
            std::snprintf(line, sizeof(line), "%.1f / %.1f MB", job->done / 1048576.0, job->total / 1048576.0);
            text.draw(vw / 2, vh / 2 - 60 * k, static_cast<int>(24 * k), white, "借りています", 1);
            text.draw(vw / 2, vh / 2 + 30 * k, static_cast<int>(18 * k), dim, line, 1);
            const float bw = 600 * k, bx = (vw - bw) / 2, by = vh / 2;
            SDL_FRect bar{bx, by, bw, 8 * k};
            SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
            SDL_RenderFillRect(r, &bar);
            if (job->total > 0) bar.w = bw * static_cast<float>(job->done) / static_cast<float>(job->total);
            else bar.w = 0;
            SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
            SDL_RenderFillRect(r, &bar);
            frame_present(r);
            SDL_Delay(16);
        }
        th.join();
        if (!job->ok) {
            std::printf("借りられない：%s\n", job->error.c_str());
            std::lock_guard<std::mutex> lock(mu);
            status = "借りられない：" + job->error;
            return false;
        }
        borrowed_folder = job->folder;
        chart_path = job->chart;
        return true;
    };
    std::function<void(const Entry*)> decide_entry;
    auto decide = [&] { decide_entry(current()); };
    decide_entry = [&](const Entry* e) {
        if (!e) return;
        if (e->kind == Entry::Kind::Folder) {
            if (e->custom && (!e->custom_loaded || e->custom->max_tracks == 1)) {
                // 開くたびに 1 曲を選び直すフォルダは、毎回集め直す（LR2: songmanage:2091）
                load_custom_entry(*lib, *e);
                folder_lamps.erase(folder_lamp_key(mode_filter, level_path(), *e));
                folder_totals.erase(folder_lamp_key(mode_filter, level_path(), *e));
            }
            if (e->children.empty()) return;  // 空のフォルダには入らない（LR2 と同じ）
            stack.push_back(make_level(e->children.ptr(), 0, e->label, e->fs_folder, e->dir, e->course_folder ? 6 : e->bar_type,
                                       e->keep_order));
            snap();
            app.mixer.play_system(kSysFolderOpen);
        } else if (e->kind == Entry::Kind::Chart || (e->kind == Entry::Kind::Course && e->course->playable())) {
            // 遊べないコース（譜面が欠けている）は、決定しても何も起きない（LR2 と同じ）
            PlayOptions po;
            po.autoplay = autoplay;
            po.replay = start_replay && e->kind == Entry::Kind::Chart;
            po.record = e->kind == Entry::Kind::Chart ? e->chart : nullptr;
            if (po.replay) po.autoplay = false;
            start_replay = false;
            // この譜面で使うと決めた、保存した配置（作り方の版が今と同じものだけ）
            if (SavedRandom sr; !po.replay && e->kind == Entry::Kind::Chart && e->chart && app.db.used_random(e->chart->sha256, sr) &&
                                sr.version == kRandomAlgorithmVersion)
                po.seed = sr.seed, po.layout_random = sr.random;
            if (borrowing) po.keep = keep_borrowed;  // 借りた譜面：リザルトで「もらう」
            if (const char* cap = dev_env("RD_PLAY_CAPTURE")) po.capture_dir = cap;  // 開発用
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
                    bgm_pending = true;
                    set_select_view();
                    return;
                }
                po.decide_sound = !app.decide_skin;
            }
            PlayExit ex = course ? run_course(app, *course, po) : run_play(app, path, po);
            back_from_play = true;
            replay_checked.clear();  // 遊んでリプレイを残したかもしれない
            // LR2 から取り込んだ仮の譜面を遊んだら、その譜面を読んで本物の情報にする（記録のキーがそろう）
            bool refreshed = false;
            if (e->chart && e->chart->sha256.rfind("md5:", 0) == 0) refreshed = app.db.refresh_chart(path);
            if (course)
                for (const CourseStage& st : course->stages)
                    if (st.chart && st.chart->sha256.rfind("md5:", 0) == 0) refreshed = app.db.refresh_chart(st.chart->path) || refreshed;
            // 借りた譜面をもらったら、持っている譜面として一覧を作り直す
            refreshed = refreshed || kept;
            kept = false;
            app.save_config();  // 演奏中に変えたハイスピードやカバーを残す
            bgm_pending = true;
            set_select_view();
            input = InputMapper(app.keycfg), input.set_min_hold_ms(app.cfg.input_interval);
            std::vector<std::string> played;
            if (course) {
                played.push_back(course->hash);
                for (const CourseStage& st : course->stages)
                    if (st.chart) played.push_back(st.chart->sha256);
            } else if (e->chart) {
                played.push_back(e->chart->sha256);
            }
            reload_after_play(refreshed, played);
            if (ex == PlayExit::Quit) quit = true;
        } else if (e->kind == Entry::Kind::Missing) {
            // 配信先にあれば借りて遊ぶ
            const TableChart& m = e->missing();
            // 一覧を取っている途中なら、取り終わるまで待つ（つながらないときも 15 秒まで）
            if (borrowable_job.valid() && borrowable_job.wait_for(std::chrono::seconds(15)) == std::future_status::ready)
                borrowable = borrowable_job.get();
            auto low = [](std::string s) {
                for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                return s;
            };
            const std::string hash = !m.sha256.empty() && borrowable.count(low(m.sha256)) ? low(m.sha256)
                                     : !m.md5.empty() && borrowable.count(low(m.md5))     ? low(m.md5)
                                                                                          : std::string();
            if (hash.empty()) {
                std::lock_guard<std::mutex> lock(mu);
                status = "この譜面は持っていない：" + m.title;
                return;
            }
            stop_preview(false);
            std::string chart_path;
            if (!run_borrow(hash, chart_path)) return;
            if (!make_chart_record(chart_path, borrowed_rec)) {
                std::lock_guard<std::mutex> lock(mu);
                status = "借りた譜面を読めない：" + m.title;
                return;
            }
            borrowed_rec.path = chart_path;
            borrowed_entry = Entry{};
            borrowed_entry.kind = Entry::Kind::Chart;
            borrowed_entry.chart = &borrowed_rec;
            borrowed_entry.label = e->label;
            borrowing = true;
            decide_entry(&borrowed_entry);
            borrowing = false;
        } else if (e->kind == Entry::Kind::ModBar) {
            // MOD に、今の一覧に見えている譜面から 1 つ選ばせる。選んだ譜面にカーソルを移して、そのまま決定する
            const ModManifest* mod = nullptr;
            for (const ModManifest* m : app.active_mods())
                if (m->id == e->mod_bar().mod_id) mod = m;
            if (!mod) return;
            Level& lv = stack.back();
            std::vector<const ChartRecord*> charts;
            for (const Entry& x : *lv.list)
                if (x.kind == Entry::Kind::Chart && x.chart) charts.push_back(x.chart);
            std::string err;
            // ルーレットのバーなら、決定したときに出ていた譜面も渡す
            const std::string shown = e->mod_bar().roulette && !roulette_title.empty() ? roulette_sha : "";
            std::string sha = mod_pick_chart(*mod, e->mod_bar(), lv.name, charts, lib->scores, mod_data_dir(""), &err, shown);
            if (!err.empty()) std::printf("MOD のバー：%s\n", err.c_str());
            for (size_t i = 0; !sha.empty() && i < lv.list->size(); ++i) {
                const Entry& x = (*lv.list)[i];
                if (x.kind == Entry::Kind::Chart && x.chart && x.chart->sha256 == sha) {
                    lv.cursor = static_cast<int>(i);
                    snap();
                    decide_chart_pending = true;
                    return;
                }
            }
        }
    };
    // 難易度の切り替え（LR2: Songselect.cpp の SELECT / →。仕様 4.7）。同じフォルダの、今より上の難易度の譜面へ移る。
    // 上が無ければ ALL に戻す
    auto change_difficulty = [&](int to = -1) {
        const Entry* e = current();
        const ChartRecord* target = e && e->kind == Entry::Kind::Chart ? e->chart : nullptr;
        if (to < 0) {
            // → / SELECT（LR2 beta3 の既定）：同じ曲フォルダに今より上の難易度があれば、その中で一番下へ。
            // 一番上にいるなら、今 ALL ならその曲の一番下の難易度、ALL でなければ ALL
            if (!target) return;
            int above = 99, lowest = 99;
            for (const ChartRecord& c : lib->charts) {
                if (c.folder != target->folder || c.difficulty < 1 || c.difficulty > 5) continue;
                if (c.difficulty > target->difficulty) above = std::min(above, c.difficulty);
                lowest = std::min(lowest, c.difficulty);
            }
            to = above != 99 ? above : difficulty_filter == 0 && lowest != 99 ? lowest : 0;
            if (to > 0)
                for (const ChartRecord& c : lib->charts)
                    if (c.folder == target->folder && c.difficulty == to) {
                        target = &c;
                        break;
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
        lv = make_level(lv.full, 0, lv.name, lv.lr2_filter, lv.dir, lv.bar_type, lv.keep_order);
        // 移った先の譜面にカーソルを合わせる。見えていなければ、同じ曲フォルダの譜面に（カーソルは同じ曲に留まる）
        int same_folder = -1;
        for (size_t i = 0; i < lv.list->size(); ++i) {
            const Entry& x = (*lv.list)[i];
            if (!target || x.kind != Entry::Kind::Chart) continue;
            if (x.chart == target) {
                lv.cursor = static_cast<int>(i);
                same_folder = -2;
                break;
            }
            if (same_folder == -1 && x.chart->folder == target->folder) same_folder = static_cast<int>(i);
        }
        if (same_folder >= 0) lv.cursor = same_folder;
        snap();
        app.mixer.play_system(app.mixer.has_system(kSysDifficulty) ? kSysDifficulty : kSysFolderOpen);
    };
    // 絞り込みやソートを変えたら、いまの一覧を作り直して、同じ要素にカーソルを合わせる
    auto reapply_view = [&](bool sound = true) {
        const Entry* e = current();
        std::string label = e ? e->label : "";
        const ChartRecord* chart = e && e->kind == Entry::Kind::Chart ? e->chart : nullptr;
        Level& lv = stack.back();
        lv = make_level(lv.full, 0, lv.name, lv.lr2_filter, lv.dir, lv.bar_type, lv.keep_order);
        for (size_t i = 0; i < lv.list->size(); ++i) {
            const Entry& x = (*lv.list)[i];
            if ((chart && x.kind == Entry::Kind::Chart && x.chart == chart) || (!chart && x.label == label)) {
                lv.cursor = static_cast<int>(i);
                break;
            }
        }
        snap();
        if (sound) app.mixer.play_system(kSysOptionChange);
    };
    // F3 + 矢印（LR2 と同じ）：↑↓ で難易度を 1〜5 の輪で、←→ でレベルを 0〜99 の輪で変えて、曲データベースに書く。
    // 難易度で絞り込んでいれば、絞り込みも新しい難易度にする
    auto edit_tag = [&](SDL_Keycode key) {
        const Entry* e = current();
        if (!e || e->kind != Entry::Kind::Chart || !e->chart) return;
        ChartRecord& c = const_cast<ChartRecord&>(*e->chart);  // lib->charts の要素
        if (key == SDLK_UP || key == SDLK_DOWN) {
            c.difficulty += key == SDLK_UP ? -1 : 1;
            if (c.difficulty < 1) c.difficulty = 5;
            if (c.difficulty > 5) c.difficulty = 1;
            if (difficulty_filter != 0) difficulty_filter = c.difficulty;
        } else {
            c.playlevel += key == SDLK_LEFT ? -1 : 1;
            if (c.playlevel < 0) c.playlevel = 99;
            if (c.playlevel > 99) c.playlevel = 0;
        }
        app.db.set_chart_tag(c.path, c.difficulty, c.playlevel);
        // 曲フォルダの一覧のバーのレベル表示を直す（難易度表のバーは表のレベルのまま）
        std::function<void(std::vector<Entry>&, bool)> retag = [&](std::vector<Entry>& list, bool in_fs) {
            for (Entry& x : list) {
                if (in_fs && x.kind == Entry::Kind::Chart && x.chart == &c) x.badge = std::to_string(c.playlevel);
                if (!x.children.empty()) retag(const_cast<Entry&>(x).children.vec(), x.fs_folder);
            }
        };
        retag(root, false);
        reapply_view();
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
    bool search_failed = false;
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
        std::set<std::string> seen;  // 同じ譜面（md5）は 1 回だけ（LR2 と同じ）
        for (const ChartRecord& c : lib->charts) {
            bool ok;
            if (bpm >= 0) ok = static_cast<int>(c.bpm_max) == bpm || static_cast<int>(c.bpm_min) == bpm;
            else if (lo >= 0) ok = lo <= c.playlevel && c.playlevel <= hi;
            else
                ok = lower(c.title).find(lq) != std::string::npos || lower(c.subtitle).find(lq) != std::string::npos ||
                     lower(c.artist).find(lq) != std::string::npos || lower(c.subartist).find(lq) != std::string::npos ||
                     lower(c.genre).find(lq) != std::string::npos;
            if (!ok || !seen.insert(c.md5).second) continue;
            if (auto f = favs.find(c.md5); f != favs.end() && f->second == 2) continue;  // IGNORE は出さない
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
        // 見つからなければ検索欄に「SEARCH FAILED」と出す（LR2 と同じ。次に入力を始めるまで）
        search_failed = hits.empty();
        if (hits.empty()) return;
        search_results.push_back(std::move(hits));
        stack.push_back(make_level(&search_results.back(), 0, name));
        snap();
        app.mixer.play_system(kSysFolderOpen);
    };
    int edit_text = 30;  // 入力している欄（30 = 検索、20〜28 = タグエディター）
    // 入力を終える。commit なら、検索欄は検索し、タグの欄はカーソルの譜面に書く（LR2: skinobject:2800）
    auto finish_edit = [&](bool commit) {
        editing = false;
        SDL_StopTextInput(app.window);
        sctx.timers.set(4, scene_ms());
        if (commit && edit_text == 30) run_search(edit_buf);
        const Entry* cur = current();
        if (commit && edit_text != 30 && cur && cur->kind == Entry::Kind::Chart && cur->chart) {
            ChartRecord& c = const_cast<ChartRecord&>(*cur->chart);  // lib->charts の要素
            bool ok = true;
            int v = std::atoi(edit_buf.c_str());
            switch (edit_text) {
            case 20: c.title = edit_buf; break;
            case 21: c.subtitle = edit_buf; break;
            case 23: c.genre = edit_buf; break;
            case 24: c.artist = edit_buf; break;
            case 25: c.subartist = edit_buf; break;
            case 27: ok = v >= 0 && v < 100; if (ok) c.playlevel = v; break;
            case 28:
                ok = v >= 0 && v <= 5;
                if (ok) {
                    c.difficulty = v;
                    if (difficulty_filter != 0) difficulty_filter = v;  // 絞り込みも追従する（LR2 と同じ）
                }
                break;
            default: ok = false; break;
            }
            if (ok) {
                app.db.set_chart_text(c.path, c.title, c.subtitle, c.genre, c.artist, c.subartist);
                app.db.set_chart_tag(c.path, c.difficulty, c.playlevel);
                rebuild();
            }
        }
        edit_buf.clear();
        edit_text = 30;
    };
    // 遅かったフレーム（80 ms 以上）は、内訳をログに残す（カクつき・固まりの原因を、遊んだあとに見られるように）。
    // 開発用：RD_TIMING があれば 50 ms 以上を出す
    const uint64_t slow_ns = dev_env("RD_TIMING") ? 50'000'000 : 80'000'000;
    uint64_t ft0 = 0, ft_banner = 0, ft_text = 0, ft_events = 0, ft_draw0 = 0, ft_draw = 0;
    int slow_logged = 0;
    uint64_t ft_swap = 0, ft_preview = 0;  // 一覧の差し替え・プレビューにかかった時間
    uint64_t lamp_ns = 0;  // このフレームでフォルダのランプを数えるのにかかった時間と、見た要素の数
    size_t lamp_nodes = 0;
    // 開発用：RD_SCROLL_TEST があれば、決まった操作（長押し・1 つずつ・フォルダを開く・戻る）を流し、
    // 1 フレームにかかった時間をまとめて出して終わる
    const bool scroll_test = dev_env("RD_SCROLL_TEST") != nullptr;
    std::vector<float> frame_ms;
    double test_t0 = -1, test_next = 0;
    int test_step = 0, test_count = 0;
    // 開発用：RD_OPEN_TEST があれば、一番上のフォルダを順に開き（中のフォルダも 8 つまで）、開くのにかかった時間と、
    // 開いてから 1 秒のうちの一番遅いフレームを出して終わる
    const bool open_test = dev_env("RD_OPEN_TEST") != nullptr;
    std::vector<int> open_pos{0};      // 階層ごとに、次に見る位置
    std::vector<int> open_done{0};     // 階層ごとに、開いた数
    bool open_measuring = false;
    double open_next = 0, open_decide_ms = 0;
    float open_max = 0;
    std::string open_label;
    size_t open_depth = 0;  // 開く前の階層の深さ（開けなかったフォルダを見分ける）
    while (!quit) {
        ft0 = SDL_GetTicksNS();
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
            if (dev_env("RD_DEBUG_INPUT") && e.type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && e.type <= SDL_EVENT_JOYSTICK_UPDATE_COMPLETE) {
                static int logged = 0;
                if (logged++ < 30) std::printf("選曲画面: ジョイスティックのイベント %u を受け取った\n", e.type);
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) std::printf("ウィンドウのフォーカスが外れた\n");
            if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_x = e.motion.x, mouse_y = e.motion.y, mouse_moved_ms = scene_ms();
            }
            auto slider_ratio = [&](const SkinObject& o) {
                const SkinSrc& src = o.src;
                SkinDraw d = eval_dst(o.dst, sctx.timers.lapse(o.dst.timer, scene_ms()));
                float len = static_cast<float>(std::max(1, src.op[1]));
                float r = src.op[0] == 0 ? (d.y + d.h / 2 - mouse_y) / len : src.op[0] == 1 ? (mouse_x - d.x - d.w / 2) / len
                          : src.op[0] == 2 ? (mouse_y - d.y - d.h / 2) / len : (d.x + d.w / 2 - mouse_x) / len;
                return std::clamp(r, 0.0f, 1.0f);
            };
            // つかんでいるスライダーを、マウスの位置に合わせる（動かしたとき・押したとき）
            auto drag_update = [&] {
                if (dragging && is_sound_slider(dragging->src.op[2])) {
                    // 音のスライダー：10〜16 = EQ（-12〜12 dB）、17〜19 = 音量 MASTER / KEY / BGM（0〜100）、
                    // 20〜25 = エフェクタ 3 つ × パラメータ 2 つ（0〜100）、26 = ピッチ（-12〜12 半音）
                    const int ty = dragging->src.op[2];
                    const float r = slider_ratio(*dragging);
                    SoundFx& fx = app.cfg.sound_fx;
                    if (ty >= 10 && ty <= 16) fx.eq[ty - 10] = static_cast<int>(std::lround(r * 24)) - 12;
                    else if (ty == 17) app.cfg.vol_master = static_cast<int>(std::lround(r * 100));
                    else if (ty == 18) app.cfg.vol_key = static_cast<int>(std::lround(r * 100));
                    else if (ty == 19) app.cfg.vol_bgm = static_cast<int>(std::lround(r * 100));
                    else if (ty >= 20 && ty <= 25) ((ty - 20) % 2 == 0 ? fx.fx[(ty - 20) / 2].p1 : fx.fx[(ty - 20) / 2].p2) = static_cast<int>(std::lround(r * 100));
                    else fx.pitch = static_cast<int>(std::lround(r * 24)) - 12;
                    app.apply_volume();
                    app.apply_sound_fx();
                } else if (dragging && dragging->src.op[2] == 7) {
                    // カスタマイズ項目のスクロール
                    const SkinSrc& src = dragging->src;
                    SkinDraw d = eval_dst(dragging->dst, sctx.timers.lapse(dragging->dst.timer, scene_ms()));
                    float len = static_cast<float>(std::max(1, src.op[1]));
                    float r = src.op[0] == 0 ? (d.y + d.h / 2 - mouse_y) / len : src.op[0] == 1 ? (mouse_x - d.x - d.w / 2) / len
                              : src.op[0] == 2 ? (mouse_y - d.y - d.h / 2) / len : (d.x + d.w / 2 - mouse_x) / len;
                    catalog.set_scroll(static_cast<int>(std::lround(std::clamp(r, 0.0f, 1.0f) * catalog.scroll_max(app.cfg))), app.cfg);
                } else if (dragging && !stack.back().list->empty()) {
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
            };
            if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) drag_update();
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && dragging &&
                (dragging->src.op[2] == 7 || is_sound_slider(dragging->src.op[2]))) {
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
            if (layout_open) {
                // 保存した配置の一覧を開いている間は、その中の操作だけ
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    switch (e.key.key) {
                    case SDLK_UP: step_layout(-1); break;
                    case SDLK_DOWN: step_layout(1); break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: if (!e.key.repeat) choose_layout(); break;
                    case SDLK_DELETE: if (!e.key.repeat) delete_layout(); break;
                    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_F9: if (!e.key.repeat) close_layouts(); break;
                    default: break;
                    }
                }
                if (e.type == SDL_EVENT_MOUSE_WHEEL) step_layout(e.wheel.y > 0 ? -1 : e.wheel.y < 0 ? 1 : 0);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) close_layouts();
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
                        // キーコンフィグ：選曲画面をフェードアウト（タイマー 2）させてから移る（LR2: Songselect.cpp:2071）。
                        // システム音を止めてオプションの音を鳴らす。移るのはフェードが終わってから（下）
                        if (!to_keyconfig) {
                            for (int id = 0; id < kSysSoundCount; ++id) app.mixer.stop_system(id);
                            app.mixer.play_system(kSysOptionChange);
                            sctx.timers.set(2, scene_ms());
                            to_keyconfig = true;
                        }
                    } else if (ty == 15 || ty == 16) {
                        // 通常 / オートで始める。曲とコースのときだけ（フォルダは開かない。LR2 と同じ）
                        const Entry* cur = current();
                        if (cur && (cur->kind == Entry::Kind::Chart || cur->kind == Entry::Kind::Course)) {
                            bool was = autoplay;
                            autoplay = ty == 16;
                            decide();
                            autoplay = was;
                        }
                    } else if (ty == 40) {
                        app.cycle_gauge(dir > 0 ? 1 : -1);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == kJudgeButton) {
                        cycle_judge(dir);
                    } else if (ty == kMetronomeButton) {
                        toggle_metronome();
                    }
                    else if (ty == 46) {
                        // LANE COVER：OFF → SUDDEN+ → LIFT → SUDDEN+ と LIFT → OFF（逆に押せば逆の順）。
                        // WMIX などには LIFT のボタンが無いので、SUDDEN+ の欄で一緒に選ぶ
                        int s = (app.cfg.lane_cover ? 1 : 0) + (app.cfg.lift ? 2 : 0);
                        s = (s + (dir < 0 ? 3 : 1)) % 4;
                        app.cfg.lane_cover = s & 1, app.cfg.lift = (s & 2) != 0;
                        app.mixer.play_system(kSysOptionChange);
                    }
                    else if (ty == 331) app.cfg.lift = !app.cfg.lift, app.mixer.play_system(kSysOptionChange);
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
                            if (catalog.selected_type() == 10) {  // サウンドセットか、その中の選択（選曲 BGM など）を替えた
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
                        // スキンセレクト画面：キーコンフィグと同じく、選曲画面をフェードアウトさせてから移る（LR2: Songselect.cpp:2071）
                        if (!to_keyconfig && !to_skin_select) {
                            for (int id = 0; id < kSysSoundCount; ++id) app.mixer.stop_system(id);
                            app.mixer.play_system(kSysOptionChange);
                            sctx.timers.set(2, scene_ms());
                            to_skin_select = true;
                        }
                        break;
                    }
                    else if (ty == 10) change_difficulty((difficulty_filter + 1) % 6);
                    else if (ty == 11 || ty == 12) {
                        // 鍵盤数の絞り込み・ソート（左右どちらでも +1。ソートは LR2 と同じく 0..4）
                        int& v = ty == 11 ? mode_filter : sort_mode;
                        v = (v + 1) % (ty == 11 ? kModeCount : kSortCount);
                        reapply_view(false);
                        // 音は難易度の切り替えの音（無ければフォルダを開く音。LR2 と同じ）
                        app.mixer.play_system(app.mixer.has_system(kSysDifficulty) ? kSysDifficulty : kSysFolderOpen);
                    }
                    else if (ty >= 91 && ty <= 96) change_difficulty(ty - 91);
                    else if (ty == 44) app.cfg.seven_key = !app.cfg.seven_key, app.mixer.play_system(kSysOptionChange);  // 7k モード
                    else if (ty == 50) loop(app.cfg.effect, 0, 3);
                    else if (ty == 72) loop(app.cfg.bga, 0, 2);  // BGA（ボタンは OFF / ON / AUTOPLAY の 3 つを回す。LR2 と同じ）
                    else if (ty == 75) loop(app.cfg.auto_adjust, 0, 2);  // 判定タイミングの自動調整（OFF / ON / SILENT）
                    else if (ty == 70) {
                        int v = app.cfg.score_graph ? 1 : 0;
                        loop(v, 0, 1);
                        app.cfg.score_graph = v != 0;
                    } else if (ty == 71) loop(app.cfg.ghost, 0, 3);
                    else if (ty == 73) loop(app.cfg.bga_size, 0, 1);
                    else if (ty >= 20 && ty <= 33 && ty != 30) {
                        // 音の設定（LR2: skinobject の 20〜33）。20〜22 エフェクタの種類、23〜25 オン／オフ、26〜28 掛け先、
                        // 29 EQ のオン／オフ、31 音量のオン／オフ、32 ピッチのオン／オフ、33 ピッチの方式
                        SoundFx& fx = app.cfg.sound_fx;
                        if (ty <= 22) fx.set_fx_type(ty - 20, (fx.fx[ty - 20].type + (dir > 0 ? 1 : 7)) % 8);
                        else if (ty <= 25) fx.fx[ty - 23].on = !fx.fx[ty - 23].on;
                        else if (ty <= 28) loop(fx.fx[ty - 26].target, 0, 2);
                        else if (ty == 29) fx.eq_on = !fx.eq_on;
                        else if (ty == 31) app.cfg.volume_on = !app.cfg.volume_on;
                        else if (ty == 32) fx.pitch_on = !fx.pitch_on;
                        else loop(fx.pitch_type, 0, 2);
                        if ((ty >= 20 && ty <= 25) || ty == 29 || ty == 31 || ty == 32) app.mixer.play_system(kSysOptionChange);
                        app.apply_volume();
                        app.apply_sound_fx();
                    }
                    else if (ty == 76) loop(app.cfg.target_percent, 50, 100);
                    else if (ty == 77) loop(app.cfg.target, 0, 5);  // ターゲット（IR の 6〜8 は無いので飛ばす）
                    else if (ty == 83) loop(app.cfg.replay_save, 0, 4);
                    else if (ty == 90) {
                        // お気に入り：なし → FAVORITE → IGNORE（LR2: favorite 0〜2）。一覧を作り直す
                        const Entry* cur = current();
                        if (cur && cur->kind == Entry::Kind::Chart) {
                            int& v = favs[cur->chart->md5];
                            v = (v + (dir > 0 ? 1 : 2)) % 3;
                            app.db.set_favorite(cur->chart->md5, v);
                            app.mixer.play_system(kSysOptionChange);
                            load_custom(*lib, app, favs);
                            rebuild();
                        }
                    }
                    else if (ty == 19) {
                        // リプレイで遊ぶ
                        const Entry* cur = current();
                        if (cur && cur->kind == Entry::Kind::Chart && sctx.replay_exists) start_replay = true, decide();
                    }
                    else if (ty == 55) {
                        loop(app.cfg.hs_fix, 0, 5);
                    } else if (ty == 57) {
                        app.cfg.hi_speed = std::clamp(app.cfg.hi_speed + dir * app.cfg.hs_margin, app.cfg.hs_min, app.cfg.hs_max);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 74) {
                        app.cfg.judge_timing = std::clamp(app.cfg.judge_timing + dir, -199, 199);
                        app.mixer.play_system(kSysOptionChange);
                    } else if (ty == 80) {
                        app.toggle_fullscreen();
                    }
                    break;
                }
                // 検索欄（文字列 30）とタグエディターの欄（20〜28）のうち edit=1 のものをクリックしたら入力を始める
                if (!clicked)
                    for (const SkinText& tx : sk->texts) {
                        const int id = tx.src.st;
                        bool tag = id == 20 || id == 21 || (id >= 23 && id <= 25) || id == 27 || id == 28;
                        if ((id != 30 && !tag) || tx.src.op[0] != 1 || tx.dst.empty()) continue;
                        if (tag && (!current() || current()->kind != Entry::Kind::Chart)) continue;
                        int tp = tx.src.op[1];
                        if (tp != 0 && tp != panel) continue;
                        SkinDraw d = eval_dst(tx.dst, sctx.timers.lapse(tx.dst.timer, t));
                        if (!d.visible) continue;
                        float x0 = tx.src.align == 1 ? d.x - d.w / 2 : tx.src.align == 2 ? d.x - d.w : d.x;
                        if (mx >= x0 && mx < x0 + d.w && my >= d.y && my < d.y + d.h) {
                            editing = true;
                            edit_text = id;
                            // タグの欄は今の値から書き換える
                            edit_buf = tag ? sctx.text(id) : std::string();
                            SDL_StartTextInput(app.window);
                            clicked = true;
                            break;
                        }
                    }
                // 曲リストのスライダーをつかむ
                if (!clicked)
                    for (const SkinObject& o : sk->sliders) {
                        int sty = o.src.op[2];
                        if ((sty != 1 && sty != 7 && !is_sound_slider(sty)) || o.src.op[3] == 1 || o.dst.empty() || !sctx.timers.on(o.dst.timer))
                            continue;
                        SkinDraw d = eval_dst(o.dst, sctx.timers.lapse(o.dst.timer, t));
                        if (!d.visible) continue;
                        float len = static_cast<float>(o.src.op[1]);
                        SDL_FRect rc{d.x, d.y, d.w, d.h};
                        if (o.src.op[0] == 0) rc.y -= len, rc.h += len;
                        else if (o.src.op[0] == 1) rc.w += len;
                        else if (o.src.op[0] == 2) rc.h += len;
                        else rc.x -= len, rc.w += len;
                        if (mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h) {
                            // 押した所まで、すぐにつまみを動かす（つかんで引っ張らなくてよい）
                            dragging = &o;
                            mouse_x = mx, mouse_y = my;
                            drag_update();
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
            // F3 を押しながら矢印：カーソルの譜面の難易度（↑↓）とレベル（←→）を書き換える（LR2 と同じ。カーソルは動かない）
            if (e.type == SDL_EVENT_KEY_DOWN && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F3] &&
                (e.key.key == SDLK_UP || e.key.key == SDLK_DOWN || e.key.key == SDLK_LEFT || e.key.key == SDLK_RIGHT)) {
                if (!e.key.repeat) edit_tag(e.key.key);
                continue;
            }
            // START のパネルを開いている間は、← → でターゲットを替え、Enter・← → で選曲は動かさない（LR2: SetBmsFilter）
            if (e.type == SDL_EVENT_KEY_DOWN && panel == 1 &&
                (e.key.key == SDLK_LEFT || e.key.key == SDLK_RIGHT || e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) {
                if (e.key.key != SDLK_RETURN && e.key.key != SDLK_KP_ENTER) {
                    app.cfg.target = (app.cfg.target + (e.key.key == SDLK_RIGHT ? 1 : 5)) % 6;  // IR の 6〜8 は無い
                    app.mixer.play_system(kSysOptionChange);
                }
                continue;
            }
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
                    // どの階層にいても終わる（LR2 と同じ）
                    if (!e.key.repeat) quit = true;
                    break;
                case SDLK_F8:
                    // いま開いている曲フォルダの下だけを読み直す（LR2 と同じく、難易度表やルートでは何もしない）
                    if (!e.key.repeat && !stack.back().dir.empty()) refresh(true, stack.back().dir);
                    break;
                case SDLK_SPACE: space_held = true; break;
                case SDLK_F9:
                    // 保存したランダムの配置の一覧（譜面のときだけ）
                    if (!e.key.repeat)
                        if (const Entry* cur = current(); cur && cur->kind == Entry::Kind::Chart && cur->chart) open_layouts(cur->chart->sha256);
                    break;
                case SDLK_F4:
                    app.toggle_fullscreen();
                    break;
                default: break;
                }
            }
            if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_SPACE) space_held = false;
            input.handle(e);
        }
        ft_events = SDL_GetTicksNS() - ft0;
        // コントローラ：白鍵で決定、黒鍵で戻る、皿で上下（LR2の選曲画面と同じ）
        uint64_t now = SDL_GetTicksNS();
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        input.collect(now, lanes, buttons);
        double t_ms = scene_ms();
        // 入力の受付開始（#STARTINPUT。LR2 のタイマー1）
        // LR2 は入力受付（タイマー 1）と同時に、曲を選んだタイマー 11 も立てる（Main.cpp:1469）。
        // 立てないと、遊んで戻ったときに曲名などがカーソルを動かすまで出ない
        if (!sctx.timers.on(1) && (!sk || t_ms > sk->startinput)) sctx.timers.set(1, t_ms), sctx.timers.set(11, t_ms);
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
            const int old_mode = mode_filter;
            if (play_option_keys(app, input, buttons)) app.mixer.play_system(kSysOptionChange);
            if (mode_filter != old_mode) reapply_view(false);
            buttons.clear();
        }
        bool accept = sctx.timers.on(1) && !readme_open && !layout_open && !editing && !to_keyconfig && !to_skin_select;
        if (layout_open) {
            // 皿で上下、白鍵で決める、2 番で閉じる
            for (const ButtonInput& b : buttons) {
                if (!b.down) continue;
                if (b.button == kScratchA) step_layout(-1);
                else if (b.button == kScratchB) step_layout(1);
                else if (b.button == kKey1 || b.button == kKey1 + 2 || b.button == kKey1 + 4 || b.button == kKey1 + 6) choose_layout();
                else if (b.button == kKey1 + 1) close_layouts();
                if (!layout_open) break;
            }
            buttons.clear();
        }
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
                // 6 番：オートで開始。リプレイがあれば押している間を数え、2 秒でリプレイ、その前に離せばオート（LR2: タイマー 172）
                const Entry* e = current();
                if (e && e->kind == Entry::Kind::Course) {
                    bool was = autoplay;
                    autoplay = true;
                    decide();
                    autoplay = was;
                } else if (e && e->kind == Entry::Kind::Chart) {
                    if (sctx.replay_exists) {
                        sctx.timers.set(172, t_ms);
                    } else {
                        bool was = autoplay;
                        autoplay = true;
                        decide();
                        autoplay = was;
                    }
                }
            } else if (b.button == kSelect) {
                change_difficulty();
            }
        }
        // 起動の画面で待ちきれなかった難易度表が、あとで新しくなっていたら一覧を作り直す（表だけ。曲フォルダは読まない）
        if (app.table_updates && app.table_updates->remaining == 0 && app.table_updates->late_changed && !busy) {
            app.table_updates->late_changed = false;
            std::printf("遅れて新しくなった難易度表があるので、一覧を作り直す\n");
            refresh(false);
        }
        // 監視で変わったフォルダを、裏の読み直しが空いているときに 1 つずつ読み直す
        for (std::string& d : watcher.take_changed())
            if (std::find(watched_dirs.begin(), watched_dirs.end(), d) == watched_dirs.end()) watched_dirs.push_back(std::move(d));
        if (!watched_dirs.empty() && !busy) {
            std::string d = std::move(watched_dirs.front());
            watched_dirs.pop_front();
            std::printf("曲フォルダが変わった：%s\n", d.c_str());
            refresh(true, d);
        }
        if (to_keyconfig && (!sk || sk->fadeout == 0 || sctx.timers.lapse(2, t_ms) > sk->fadeout)) {
            to_keyconfig = false;
            if (!open_keyconfig_from_select(app, text, sctx, scene_ms, true)) quit = true;
            input = InputMapper(app.keycfg), input.set_min_hold_ms(app.cfg.input_interval);
            set_select_view();
        }
        if (to_skin_select && (!sk || sk->fadeout == 0 || sctx.timers.lapse(2, t_ms) > sk->fadeout)) {
            to_skin_select = false;
            set_panel(-1, false);
            bool select_changed = run_skin_select(app, catalog);
            app.save_config();
            if (select_changed) reload_select_skin();
            else set_select_view();
            app.mixer.play_system(kSysSelect, true);
        }
        if (decide_chart_pending) {
            decide_chart_pending = false;
            decide();
        }
        if (sctx.timers.on(172)) {
            const Entry* e = current();
            if (!e || e->kind != Entry::Kind::Chart || !sctx.replay_exists) {
                sctx.timers.reset(172);
            } else if (!input.button_held(kKey1 + 5)) {
                sctx.timers.reset(172);
                bool was = autoplay;
                autoplay = true;
                decide();
                autoplay = was;
            } else if (sctx.timers.lapse(172, t_ms) > 2000) {
                sctx.timers.reset(172);
                start_replay = true;
                decide();
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
        const uint64_t fsw0 = SDL_GetTicksNS();
        // 7k モードを切り替えたら、皿のある 7 鍵の譜面とコースの記録を読み直す（鍵盤 6 でも欄のクリックでも）
        if (app.cfg.seven_key != (profile.seven_key >= 0)) {
            profile = record_profile(app.db, app);
            reload_records_async();
        }
        if (records_reload_wanted && !busy) reload_records_async();
        if (attr_ready && !busy) {
            // 分析し終えた傾向で、MOD のフォルダを作り直す
            attr_ready = false;
            lib->attrs.reset();
            reload_records_async();
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            if (pending) {
                // 前の一覧は、作り直しが終わるまで残す（いまのカーソルの曲を、前の一覧の譜面から名前で探すため。
                // 先に捨てると、捨てた譜面を読んで落ちる）
                std::shared_ptr<Library> old_lib = lib;
                lib = pending;
                pending.reset();
                auto prebuilt = std::move(pending_root);
                pending_root.reset();
                if (!prebuilt) load_custom(*lib, app, favs);
                rebuild(prebuilt);
                // 前の一覧（何万もの譜面）を捨てるのは裏で（表で捨てると数十 ms 止まる）
                std::thread([l = std::move(old_lib)]() mutable { l.reset(); }).detach();
            }
        }

        ft_swap = SDL_GetTicksNS() - fsw0;
        for (const std::string& nm : joystick_init().open_new()) {
            std::printf("コントローラー接続: %s\n", nm.c_str());
            joy_names.push_back(nm);
        }
        if (joystick_init().ready() && !joy_reported) {
            std::printf("コントローラーの準備が終わった（%dms）\n", joystick_init().took_ms());
            joy_reported = true;
        }
        // プレビュー
        const uint64_t fpv0 = SDL_GetTicksNS();
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
                            schedule.push_back({frame_of(n.time_us), n.sound, true});
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
                const bool pcm16 = app.cfg.sound_16bit;
                preview_load = std::async(std::launch::async, [want, rate, pcm16]() -> std::shared_ptr<Preview> {
                    BmsParseOptions popt;
                    popt.random_seed = 1;
                    BmsParseResult parsed = parse_chart_file(want, popt);
                    if (!parsed.ok) return nullptr;
                    auto p = std::make_shared<Preview>();
                    p->path = want;
                    p->chart = std::move(parsed.chart);
                    std::vector<std::string> missing;
                    std::vector<std::string> paths = resolve_chart_sounds(p->chart, missing);
                    auto ranges = sound_ranges(p->chart);
                    p->bank.load(paths, rate, static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2)), nullptr,
                                 nullptr, &ranges, pcm16);
                    return p;
                });
            }
        }

        ft_preview = SDL_GetTicksNS() - fpv0;
        // 描画
        if (sk) {
            const Level& lv = stack.back();
            const std::vector<Entry>& list = *lv.list;
            int n = static_cast<int>(list.size());
            const Entry* cur = current();
            sctx.type = 1, sctx.chart = nullptr, sctx.score = nullptr, sctx.course = nullptr;
            sctx.artist_note.clear();
            // フォルダ（とフォルダの名前のバー）にカーソルがあれば、中の持っている譜面の記録を合計してスコアの欄に出す。
            // 同じ譜面が 2 つのフォルダにあっても 1 回だけ数える（難易度表の「全曲」とレベルのフォルダなど）
            sctx.folder_score = nullptr;
            folder_played.clear();
            if (!folder_totals_pending.empty()) folder_totals.erase(folder_totals_pending), folder_totals_pending.clear();
            if (cur && ((cur->kind == Entry::Kind::Folder && !cur->course_folder) ||
                        (cur->kind == Entry::Kind::Header && cur->bar_type != 6))) {
                const bool head = cur->kind == Entry::Kind::Header;
                std::string key = head ? folder_lamp_key(mode_filter, level_path(), Entry{}) + "\x1fhead"
                                       : folder_lamp_key(mode_filter, level_path(), *cur);
                auto [it, fresh] = folder_totals.try_emplace(key);
                bool complete = true;
                if (fresh) {
                    FolderTotal& ft = it->second;
                    ft.sc.bp = 0;
                    // 何万もの譜面を見るので、同じ譜面かどうかは番号で、記録は前もって並べたものから見る（文字列で探さない）
                    std::vector<char> seen(lib->charts.size(), 0);
                    std::unordered_set<std::string> seen_outside;
                    std::function<void(const Entry&)> walk = [&](const Entry& x) {
                        if (x.custom && !custom_ready(x)) complete = false;
                        int idx = -1;
                        const ScoreRecord* rec = x.kind == Entry::Kind::Chart && x.chart ? chart_score(x.chart, &idx) : nullptr;
                        bool first = false;
                        if (x.kind == Entry::Kind::Chart && x.chart) {
                            if (idx >= 0) {
                                char& s = seen[lib->same_sha[idx]];
                                first = !s, s = 1;
                            } else {
                                first = seen_outside.insert(x.chart->sha256).second;
                            }
                        }
                        if (first && mode_shows(mode_filter, x.chart->keys)) {
                            const int n = sctx.notes_of(*x.chart);
                            ft.notes += n;
                            ft.charts++;
                            if (rec && rec->playcount > 0) {
                                const ScoreRecord& r = *rec;
                                ft.played_notes += n;
                                ft.played++;
                                ft.sc.exscore += r.exscore;
                                if (r.bp > 0) ft.sc.bp += r.bp;
                                ft.sc.playcount += r.playcount;
                                ft.sc.clearcount += r.clearcount;
                                for (int j = 0; j < 5; ++j) ft.sc.judge[j] += r.judge[j];
                            }
                        }
                        if (!x.course_folder)
                            for (const Entry& c : x.children) walk(c);
                    };
                    if (head) {
                        for (const Entry& x : list) walk(x);
                    } else {
                        walk(*cur);
                    }
                }
                sctx.folder_score = &it->second.sc;
                sctx.folder_notes = it->second.notes, sctx.folder_played_notes = it->second.played_notes;
                if (!complete) folder_totals_pending = key;  // 集めている途中：出すが、次のフレームで数え直す
                folder_played = {it->second.played, it->second.charts,
                                 it->second.charts ? it->second.played * 100 / it->second.charts : 0};
            }
            // 曲でない要素の文字の欄（LR2 と同じく、フォルダならタイトルの枠にフォルダの名前）
            sctx.bar_text = {};
            if (cur && (cur->kind == Entry::Kind::Folder || cur->kind == Entry::Kind::Header || cur->kind == Entry::Kind::ModBar)) {
                sctx.bar_text[0] = sctx.bar_text[2] = cur->label;
                if (cur->custom) sctx.bar_text[3] = cur->custom->category;  // .lr2folder の #CATEGORY はジャンルの欄
            } else if (cur && cur->kind == Entry::Kind::Missing) {
                sctx.bar_text[0] = sctx.bar_text[2] = cur->missing().title;
                const std::string& lv = lib->table_levels(cur->missing().md5);
                sctx.bar_text[4] = lv.empty() ? cur->missing().artist : lv + " / " + cur->missing().artist;
            }
            // MOD のバーのルーレット：カーソルを置いている間、曲名の枠に一覧の曲名を毎フレーム（16ms ごと）次々に出す。
            // バーに来たときに一覧から 48 曲を選んでおき、その中から出す（毎フレーム違う曲名の文字を作ると重い。
            // 48 曲なら、一回りしたあとは作った文字を使い回せる）
            sctx.title_override.clear();
            if (cur && cur->kind == Entry::Kind::ModBar && cur->mod_bar().roulette) {
                if (roulette_pool.empty()) {
                    std::vector<const ChartRecord*> charts;
                    for (const Entry& x : list)
                        if (x.kind == Entry::Kind::Chart && x.chart) charts.push_back(x.chart);
                    std::shuffle(charts.begin(), charts.end(), roulette_rng);
                    if (charts.size() > 48) charts.resize(48);
                    for (const ChartRecord* c : charts)
                        roulette_pool.push_back({c->title + (c->subtitle.empty() ? "" : " " + c->subtitle), c->sha256});
                }
                if (!roulette_pool.empty() && (t_ms - roulette_ms >= 16 || roulette_title.empty())) {
                    const auto& [title, sha] = roulette_pool[std::uniform_int_distribution<size_t>(0, roulette_pool.size() - 1)(roulette_rng)];
                    roulette_title = title, roulette_sha = sha;
                    roulette_ms = t_ms;
                }
                sctx.title_override = roulette_title;
            } else {
                roulette_title.clear(), roulette_sha.clear();  // 別のフォルダの譜面を次のルーレットに残さない
                roulette_pool.clear();
            }
            if (cur && ((cur->kind == Entry::Kind::Folder && cur->course_folder) ||
                        (cur->kind == Entry::Kind::Header && cur->bar_type == 6))) {
                sctx.type = 6;
            } else if (cur && cur->kind == Entry::Kind::Course) {
                sctx.type = 8;
                sctx.course = cur->course.get();
                auto it = lib->scores.find(cur->course->hash);
                if (it != lib->scores.end()) sctx.score = &it->second;
                if (dev_env("RD_DEBUG_COURSE")) {
                    static std::string last;
                    if (last != cur->course->hash) {
                        last = cur->course->hash;
                        std::printf("[コース] %s 記録 %s／EX %d／最大コンボ %d／BP %d／ノーツ %d\n", cur->course->name.c_str(), sctx.score ? "あり" : "なし",
                                    sctx.number(71), sctx.number(75), sctx.number(76), sctx.number(74));
                    }
                }
            } else if (cur && cur->kind == Entry::Kind::Chart) {
                sctx.type = 0;
                sctx.chart = cur->chart;
                // アーティストの左に、入っている難易度表のレベルと MOD の label（空白だけだとスキンの字では詰まって見えるので / で区切る）
                sctx.artist_note = lib->table_levels(cur->chart->md5);
                if (!cur->note.empty()) sctx.artist_note += (sctx.artist_note.empty() ? "" : " / ") + cur->note;
                auto it = lib->scores.find(cur->chart->sha256);
                if (it != lib->scores.end()) sctx.score = &it->second;
            } else if (cur && cur->kind == Entry::Kind::Missing) {
                sctx.type = 0;
            }
            // 同じフォルダの、難易度ごとの譜面（WMIX の NORMAL / HYPER / ANOTHER の枠）
            sctx.diff_level.fill(-1), sctx.diff_count.fill(0), sctx.diff_lamp.fill(0);
            if (sctx.chart) {
                if (lib->by_folder.empty())
                    for (const ChartRecord& c : lib->charts) lib->by_folder[c.folder].push_back(&c);
                auto fit = lib->by_folder.find(sctx.chart->folder);
                if (fit != lib->by_folder.end())
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
            const uint64_t ftb = SDL_GetTicksNS();
            {
                std::string want = sctx.chart ? banner_path_of(*sctx.chart) : "";
                if (want != banner_want) {
                    banner_want = want;
                    if (banner_tex) app.select_renderer->set_image_override(102, nullptr), SDL_DestroyTexture(banner_tex), banner_tex = nullptr;
                    if (!want.empty()) {
                        std::lock_guard<std::mutex> lock(banner->mu);
                        banner->want = want;
                        banner->cv.notify_one();
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
            ft_banner = SDL_GetTicksNS() - ftb;
            const uint64_t ftt = SDL_GetTicksNS();
            sctx.has_text = false;
            if (sctx.chart) {
                // 分かっていなければ裏で確かめてもらい、分かるまでは「無し」として描く
                std::lock_guard<std::mutex> lock(banner->mu);
                auto it = banner->folder_text.find(sctx.chart->folder);
                if (it != banner->folder_text.end()) sctx.has_text = it->second;
                else if (banner->want_folder != sctx.chart->folder) banner->want_folder = sctx.chart->folder, banner->cv.notify_one();
                ft_text = SDL_GetTicksNS() - ftt;
            }
            if (editing && edit_text == 30) search_failed = false;
            sctx.folder_name = editing && edit_text == 30 ? edit_buf + "|" : search_failed ? "SEARCH FAILED" : stack.size() > 1 ? lv.name : "検索語句を入力";
            sctx.edit_text = editing && edit_text != 30 ? edit_text : -1;
            sctx.edit_value = edit_buf;
            sctx.panel = panel;
            sctx.difficulty_filter = difficulty_filter;
            sctx.mode_filter = mode_filter;
            sctx.sort_mode = sort_mode;
            sctx.mode_name = kModeNames[mode_filter];
            sctx.sort_name = kSortNames[sort_mode];
            sctx.autoplay = autoplay;
            // カスタムゲージを選んでいれば、その名前と、ゲージのボタンの絵（落ちるものは HARD、落ちないものは GROOVE の絵）
            sctx.custom_gauge_name.clear();
            sctx.custom_gauge_frame = -1;
            // 判定の欄：使っているカスタム判定の MOD で遊ぶ判定の、一覧の何番目か（選んでいなければ OFF）
            sctx.no_record = app.metronome() != nullptr;
            sctx.judge_frame = 0;
            for (const ModManifest* m : app.active_mods())
                if (m->has_setting("judges")) sctx.judge_frame = judge_choice(*m);
            for (const auto& [key, g] : app.custom_gauges())
                if (key == app.cfg.custom_gauge) sctx.custom_gauge_name = g->name, sctx.custom_gauge_frame = g->survival ? kHard : kGroove;
            // GAS：名前は「GAS」、ゲージの枠は順番の先頭のゲージのもの
            if (app.cfg.custom_gauge == kGasKey) {
                const std::vector<std::string> order = app.gas_order();
                if (!order.empty()) {
                    sctx.custom_gauge_name = "GAS";
                    sctx.custom_gauge_frame = kHard;
                    for (int i = 0; i < kGaugeCount; ++i)
                        if (order[0] == kGaugeKeys[i]) sctx.custom_gauge_frame = i == kGroove || i == kEasy ? kGroove : kHard;
                    for (const auto& [key, g] : app.custom_gauges())
                        if (key == order[0]) sctx.custom_gauge_frame = g->survival ? kHard : kGroove;
                }
            }
            if (auto f = sctx.type == 0 && sctx.chart ? favs.find(sctx.chart->md5) : favs.end(); f != favs.end()) sctx.favorite = f->second;
            else sctx.favorite = 0;
            {
                // カーソルの譜面にリプレイがあるか（譜面が変わったときだけファイルを見る）
                const ChartRecord* c = sctx.type == 0 ? sctx.chart : nullptr;
                std::string sha = c ? c->sha256 : std::string();
                if (sha.empty() || sha != replay_checked) {
                    std::error_code ec;
                    replay_found = !sha.empty() && fs::exists(replay_path(sha), ec);
                    replay_checked = sha;
                }
                sctx.replay_exists = replay_found;
            }
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
            const int64_t new_since_now = new_song_since(app.cfg.title_flash);
            const std::string lamp_path = level_path();
            const auto lamp_defs = app.custom_lamps();
            auto make_bar = [&](int i) {
                SelectBar b;
                const Entry& en = list[i];
                if (en.kind == Entry::Kind::Folder) {
                    b.type = en.course_folder ? 6 : en.bar_type;
                    b.title = en.label;
                    if (app.cfg.folder_lamp && !en.course_folder) {
                        // 持っている譜面だけを見る（鍵盤数の絞り込みは効かせる）。PERFECT から始めて一番低いランプ
                        auto [it, fresh] = folder_lamps.try_emplace(folder_lamp_key(mode_filter, lamp_path, en), -1);
                        if (fresh) {
                            const uint64_t lt0 = SDL_GetTicksNS();
                            int lamp = 7;
                            bool complete = true;
                            // 関数を呼ばずに積んで回る（何万もの要素を見るので、1 つあたりの手間を小さく）。
                            // 中身を集めるカスタムフォルダだけ、集め終わっているかを確かめる
                            std::vector<const Entry*> todo{&en};
                            while (!todo.empty()) {
                                const Entry& x = *todo.back();
                                todo.pop_back();
                                ++lamp_nodes;
                                if (x.custom && !custom_ready(x)) complete = false;
                                if (x.kind == Entry::Kind::Chart) {
                                    if (x.chart && mode_shows(mode_filter, x.chart->keys)) lamp = std::min(lamp, chart_lamp(x.chart));
                                    continue;
                                }
                                for (const Entry& c : x.children) todo.push_back(&c);
                            }
                            lamp_ns += SDL_GetTicksNS() - lt0;
                            it->second = lamp == 7 ? 0 : std::min(lamp, 6);
                            if (!complete) {
                                // 中身を集めている途中のカスタムフォルダがある：今は出さず、次のフレームで数え直す
                                folder_lamps.erase(it);
                                return b;
                            }
                        }
                        b.lamp = it->second;
                    }
                } else if (en.kind == Entry::Kind::Course) {
                    // コースのバーはレベルを出さず、ランプはコースの記録
                    b.type = 8;
                    b.title = en.label;
                    auto it = lib->scores.find(en.course->hash);
                    if (it != lib->scores.end()) b.lamp = it->second.clear;
                } else if (en.kind == Entry::Kind::Header || en.kind == Entry::Kind::ModBar) {
                    // フォルダの名前のバーは、そのフォルダと同じ色
                    b.type = en.bar_type;
                    b.title = en.label;
                } else {
                    b.type = 0;
                    b.title = en.label;
                    if (en.kind == Entry::Kind::Chart) {
                        b.is_new = en.chart->added > 0 && en.chart->added > new_since_now;  // 新曲の色（LR2: adddate）
                        int df = en.chart->difficulty;
                        b.level_index = df >= 1 && df <= 5 ? df : 0;
                        b.level = en.chart->playlevel;
                        auto it = lib->scores.find(en.chart->sha256);
                        if (it != lib->scores.end()) b.lamp = it->second.clear;
                        // カスタムランプ：ふだんのランプより上の並びのものがあれば、その見せ方の絵をランプの色に染めて出す
                        double best = b.lamp;
                        if (auto cl = lib->custom_lamps.find(en.chart->sha256); cl != lib->custom_lamps.end())
                            for (const auto& [key, v] : cl->second) {
                                auto d = lamp_defs.find(key);
                                if (v < 2 || d == lamp_defs.end() || d->second->rank <= best) continue;
                                best = d->second->rank;
                                b.lamp = std::clamp(d->second->as, 1, 4);
                                b.lamp_tint = 0xff000000u | d->second->color_at(t_ms);
                            }
                    }
                }
                return b;
            };
            // 本体のランプの色を変えていれば、その色に染める（カスタムランプの色が付いていないときだけ）
            ss.bar = [&](int i) {
                SelectBar b = make_bar(i);
                if (!b.lamp_tint) b.lamp_tint = app.standard_lamp_tint(b.lamp, t_ms);
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
            // ゲージのボタン（40）：GAS・カスタムゲージは、スキンの絵に無いので名前を書く
            st.button_label = [&](int ty) { return ty == 40 ? sctx.custom_gauge_name : std::string(); };
            // GAUGE のボタン：GAS は 7 コマ目、カスタムゲージは MOD に書いたコマ（スキンの絵にあれば）
            st.button_extra_frame = [&](int ty) {
                if (ty != 40 || app.cfg.custom_gauge.empty()) return -1;
                if (app.cfg.custom_gauge == kGasKey) return kGaugeCount;
                for (const auto& [key, g] : app.custom_gauges())
                    if (key == app.cfg.custom_gauge) return g->frame;
                return -1;
            };
            st.select = &ss;
            // フォルダの合計とコースは、スキンの桁より大きい数になる（コースの最大 EX など）。字の幅を詰めて収める
            st.grow_digits = sctx.folder_score != nullptr || sctx.type == 8;
            // WMIX のスコアのパネルの背景（pannel.png の 543,453 からの 512×399）。他のスキンなら無い
            const SkinObject* score_panel = nullptr;
            for (const SkinObject& o : sk->image_objs)
                if (o.src.valid && o.src.x == 543 && o.src.y == 453 && o.src.w == 512 && o.src.h == 399 && o.dst.timer == 23)
                    score_panel = &o;
            // 遊んだ譜面の数と割合は、PLAY COUNT（プレイ回数の数字）の右に添える
            st.number_after = [&](int id) {
                std::vector<PlaySkinState::NumberExtra> out;
                if (id != 77 || folder_played.size() != 3) return out;
                // 「遊んだ数 / 持っている数（割合%）」。数字は TOTAL と同じ絵。
                // 「/」「(」「%)」は、WMIX ではスコアのパネルの背景の絵に描いてあるので、そこから切り抜いて使う
                const float cw = 12;  // 1 文字の幅（WMIX の数字）
                const float x1 = cw * 7, x2 = x1 + cw * 5 + cw, x3 = x2 + cw * 5 + cw * 2;
                auto number = [&](int v, int digits, float x) {
                    PlaySkinState::NumberExtra e;
                    e.value = v, e.digits = digits, e.dx = x;
                    out.push_back(e);
                };
                auto glyph = [&](int sx, int sy, int w, int h, float x, float y) {
                    if (!score_panel) return;
                    PlaySkinState::NumberExtra e;
                    e.image.valid = true, e.image.gr = score_panel->src.gr;
                    e.image.x = score_panel->src.x + sx, e.image.y = score_panel->src.y + sy, e.image.w = w, e.image.h = h;
                    e.dx = x, e.dy = y;
                    out.push_back(e);
                };
                number(folder_played[0], 5, x1);
                glyph(356, 56, 12, 17, x2 - cw, -4);  // 「/」（EX の最大の前のもの）
                number(folder_played[1], 5, x2);
                glyph(164, 39, 8, 17, x3 - 6, -3);  // 「(」（PGREAT の割合の前のもの）
                number(folder_played[2], 3, x3);
                glyph(207, 39, 22, 17, x3 + cw * 3 + 1, -3);  // 「%)」
                return out;
            };
            // スキンがマウスカーソルを描くときは、OS のカーソルを隠す（二重に出ないように）
            bool skin_cursor = sk->mousecursor.src.valid && !sk->mousecursor.dst.empty();
            if (skin_cursor && SDL_CursorVisible()) SDL_HideCursor();
            else if (!skin_cursor && !SDL_CursorVisible()) SDL_ShowCursor();
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            ft_draw0 = SDL_GetTicksNS();
            app.select_renderer->draw_select(*sk, st);
            ft_draw = SDL_GetTicksNS() - ft_draw0;
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
                text.draw(px, 70, 24, dim, cur->missing().title, 0, 420);
                text.draw(px, 104, 17, dim, cur->missing().artist, 0, 420);
                text.draw(px, 140, 16, white, "この譜面は手元に無い");
                if (!cur->missing().url.empty()) text.draw(px, 168, 13, dim, "本体：" + cur->missing().url, 0, 420);
                if (!cur->missing().url_diff.empty()) text.draw(px, 188, 13, dim, "差分：" + cur->missing().url_diff, 0, 420);
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
                  "↑↓ 選ぶ　Enter/→ 決定　← 戻る　Esc 終了　START/Space オプション　F1 キーの説明　（コントローラ：白鍵 決定／黒鍵 戻る／皿 選ぶ）");
        if (dev_env("RD_DEBUG_LOOP")) {
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
        if (layout_open) {
            // 保存した配置の一覧（保存した日時とオプション）。この譜面で使う配置は色を変える
            const float vw = sk ? static_cast<float>(sk->width) : 1280.0f, vh = sk ? static_cast<float>(sk->height) : 720.0f;
            const float k = vh / 720.0f, row = 34 * k, w = 420 * k;
            const int shown = std::min(static_cast<int>(layouts.size()) + 1, 12);
            const int top = std::clamp(layout_cursor - shown / 2, 0, static_cast<int>(layouts.size()) + 1 - shown);
            const float h = (shown + 1.5f) * row, x = (vw - w) / 2, y = (vh - h) / 2;
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 245);
            SDL_FRect bg{x, y, w, h};
            SDL_RenderFillRect(r, &bg);
            SDL_SetRenderDrawColor(r, 90, 90, 90, 255);
            SDL_RenderRect(r, &bg);
            const SDL_Color white{235, 235, 240, 255}, dim{140, 140, 150, 255}, accent{255, 210, 90, 255};
            const int fs = static_cast<int>(18 * k);
            text.draw(x + 16 * k, y + 8 * k, fs, dim, "保存した配置");
            static const char* names[] = {"OFF", "MIRROR", "RANDOM", "S-RANDOM", "SCATTER", "CONVERGE"};
            bool any_used = false;
            for (const SavedRandom& s : layouts) any_used = any_used || s.used;
            for (int i = top; i < top + shown; ++i) {
                const float ry = y + (i - top + 1.2f) * row;
                if (i == layout_cursor) {
                    SDL_SetRenderDrawColor(r, 60, 60, 70, 255);
                    SDL_FRect rc{x + 4 * k, ry, w - 8 * k, row - 2 * k};
                    SDL_RenderFillRect(r, &rc);
                }
                const bool used = i == 0 ? !any_used : layouts[i - 1].used;
                std::string label = "使わない", opt;
                if (i > 0) {
                    const SavedRandom& s = layouts[i - 1];
                    std::time_t tt = static_cast<std::time_t>(s.saved_at);
                    std::tm tm{};
#ifdef _WIN32
                    localtime_s(&tm, &tt);
#else
                    localtime_r(&tt, &tm);
#endif
                    char buf[32];
                    std::strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tm);
                    label = buf;
                    opt = names[std::clamp(s.random, 0, 5)];
                }
                text.draw(x + 24 * k, ry + 6 * k, fs, used ? accent : white, label);
                if (!opt.empty()) text.draw(x + w - 24 * k, ry + 6 * k, fs, dim, opt, 2);
            }
        }
        // F1 を押している間は F キーの説明（LR2 と同じ）。RD に無い操作は灰色
        if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1]) {
            const SDL_Color on{255, 255, 255, 255}, off{110, 110, 110, 255};
            const Entry* cur = current();
            bool course = cur && cur->kind == Entry::Kind::Course;
            SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_FRect bg{0, 0, 330, 76};
            SDL_RenderFillRect(r, &bg);
            float x = 4;
            x = 4 + text.draw(x, 2, 14, off, "F2 マニアックオプション　");
            text.draw(x, 2, 14, course ? off : on, course ? "F3 コースのソート変更" : "F3 レベルの変更");
            x = 4 + text.draw(4, 20, 14, on, "F4 ウインドウモード切り替え　");
            text.draw(x, 20, 14, off, "F5 IRに接続");
            x = 4 + text.draw(4, 38, 14, on, "F6 スクリーンショット　");
            text.draw(x, 38, 14, on, "F7 FPS表示");
            x = 4 + text.draw(4, 56, 14, on, "F8 フォルダのリロード　");
            text.draw(x, 56, 14, course ? off : on, "F9 保存した配置");
        }
        bool settled;
        {
            std::lock_guard<std::mutex> lock(mu);
            settled = !busy && !pending;
        }
        if (open_test && settled && scene_ms() >= open_next) {
            const double now_ms = scene_ms();
            if (open_next == 0) {
                open_next = now_ms + 2000;  // 落ち着くまで
            } else if (open_measuring) {
                std::printf("[開く] %s%s：開く %.0f ms、そのあと一番遅いフレーム %.0f ms\n", std::string(2 * (stack.size() - 2), ' ').c_str(),
                            open_label.c_str(), open_decide_ms, open_max);
                open_measuring = false;
                if (stack.size() == open_depth) {
                    ++open_pos.back();  // 開かなかった（空のフォルダなど）
                } else if (stack.size() < 3) {
                    open_pos.push_back(0), open_done.push_back(0);  // 中のフォルダも開く
                } else {
                    back();
                    ++open_pos.back();
                }
                open_next = now_ms + 300;
            } else {
                const std::vector<Entry>& list = *stack.back().list;
                int k = open_pos.back();
                while (k < static_cast<int>(list.size()) && list[k].kind != Entry::Kind::Folder) ++k;
                const bool limit = stack.size() > 1 && open_done.back() >= 8;
                if (k >= static_cast<int>(list.size()) || limit) {
                    if (stack.size() == 1) {
                        std::printf("[開く] 終わり\n");
                        quit = true;
                    } else {
                        back();
                        open_pos.pop_back(), open_done.pop_back();
                        ++open_pos.back();
                        open_next = now_ms + 300;
                    }
                } else {
                    open_pos.back() = k;
                    ++open_done.back();
                    stack.back().cursor = k;
                    snap();
                    open_label = list[k].label;
                    open_depth = stack.size();
                    const uint64_t d0 = SDL_GetTicksNS();
                    decide();
                    open_decide_ms = (SDL_GetTicksNS() - d0) / 1e6;
                    open_max = 0;
                    open_measuring = true;
                    open_next = now_ms + 1000;
                }
            }
        }
        if (scroll_test && settled) {
            // 0: 落ち着くまで 2 秒 → 1: 下を 8 秒長押し → 2: 1 つずつ 30 回（250 ms ごと）→ 3: 決定（フォルダを開く）→
            // 4: 中で 8 秒長押し → 5: 1 つずつ 30 回 → 6: 戻る → 7: 上を 5 秒長押し → 終わり
            const double now_ms = scene_ms();
            if (test_t0 < 0) test_t0 = now_ms, test_next = now_ms + 2000, frame_ms.clear();
            if (now_ms >= test_next) {
                switch (test_step) {
                case 0: case 3: down_held = true, test_next = now_ms + 8000, std::printf("[試験] 下を長押し\n"); break;
                case 1: case 4:
                    down_held = false;
                    move(1);
                    test_next = now_ms + 250;
                    if (++test_count < 30) --test_step;
                    else test_count = 0, std::printf("[試験] 1 つずつ 30 回\n");
                    break;
                case 2:
                    // フォルダを開き、中の最初のフォルダ（難易度表なら「全曲」）も開いて、曲が並ぶ一覧にする。
                    // RD_SCROLL_TEST に名前を書けば、一番上のその名前を含むフォルダを開く
                    if (const char* want = dev_env("RD_SCROLL_TEST"); want && std::string(want) != "1") {
                        const std::vector<Entry>& top = *stack.back().list;
                        for (size_t i = 0; i < top.size(); ++i)
                            if (top[i].kind == Entry::Kind::Folder && top[i].label.find(want) != std::string::npos) {
                                stack.back().cursor = static_cast<int>(i), snap();
                                break;
                            }
                    }
                    decide();
                    {
                        const std::vector<Entry>& in = *stack.back().list;
                        for (size_t i = 0; i < in.size(); ++i)
                            if (in[i].kind == Entry::Kind::Folder) {
                                stack.back().cursor = static_cast<int>(i), snap(), decide();
                                break;
                            }
                    }
                    test_next = now_ms + 1500;
                    std::printf("[試験] 決定（%s、%zu 曲）\n", stack.back().name.c_str(), stack.back().list->size());
                    break;
                case 5: back(), back(), test_next = now_ms + 1500, std::printf("[試験] 戻る\n"); break;
                case 6: up_held = true, test_next = now_ms + 5000, std::printf("[試験] 上を長押し\n"); break;
                default: {
                    up_held = false;
                    std::vector<float> v = frame_ms;
                    std::sort(v.begin(), v.end());
                    auto pct = [&](double p) { return v.empty() ? 0.0f : v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
                    int over33 = 0, over100 = 0;
                    for (float f : v) over33 += f > 33, over100 += f > 100;
                    std::printf("[試験] %zu フレーム：中央 %.1f ms、99%% %.1f ms、最大 %.1f ms、33 ms 超 %d、100 ms 超 %d\n", v.size(), pct(0.5),
                                pct(0.99), v.empty() ? 0.0f : v.back(), over33, over100);
                    quit = true;
                    break;
                }
                }
                ++test_step;
            }
        }
        if (const char* shot = dev_env("RD_SELECT_SHOT"); shot && settled) {
            // 開発用：更新が終わってから1枚保存して終わる。RD_SELECT_KEYS の文字で操作してから撮る（d=下 e=決定 a=オートで決定）
            static size_t step = 0;
            const char* keys = dev_env("RD_SELECT_KEYS");
            if (keys && keys[step]) {
                // 開発用：RD_SELECT_FRAMES を指定すると、操作している間の毎フレームを保存する
                if (const char* dir = dev_env("RD_SELECT_FRAMES")) {
                    static int frame_no = 0;
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/f%04d.bmp", dir, frame_no++);
                    if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, name), SDL_DestroySurface(sf);
                }
                char k = keys[step++];
                if (k == 'd') move(1);
                else if (k == 'D') move(10);
                else if (k == 'u') move(-1);
                else if (k == 'e') decide();
                else if (k == 'a') {
                    // オートで開始（6 番と同じ）
                    bool was = autoplay;
                    autoplay = true;
                    decide();
                    autoplay = was;
                }
                else if (k == 'b') back();
                else if (k == 'k') run_key_wizard(r, text, app.keycfg, keyconfig_path());
                else if (k == 'K' && !to_keyconfig) {
                    // ボタン 13 と同じ（フェードアウトしてからスキンのキーコンフィグ）
                    app.mixer.play_system(kSysOptionChange);
                    sctx.timers.set(2, scene_ms());
                    to_keyconfig = true;
                }
                else if (k == 'f') {
                    // RD_SELECT_FIND の名前を含むコース・譜面を探し、そこまでフォルダを開いてカーソルを置く
                    // 「@ファイル」なら、ファイルに書いた名前（UTF-8）。日本語は環境変数で渡すと化けることがある
                    std::string want = dev_env("RD_SELECT_FIND") ? dev_env("RD_SELECT_FIND") : "";
                    if (!want.empty() && want[0] == '@') {
                        std::ifstream f(fs::u8path(want.substr(1)), std::ios::binary);
                        std::getline(f, want);
                        if (want.rfind("\xEF\xBB\xBF", 0) == 0) want.erase(0, 3);
                        while (!want.empty() && (want.back() == '\r' || want.back() == ' ')) want.pop_back();
                    }
                    std::vector<std::string> chain;  // 開くフォルダの名前と、最後に目当ての名前
                    // 「folder:」で始まれば、> で区切ったフォルダの名前を順に開く
                    if (want.rfind("folder:", 0) == 0) {
                        std::string rest = want.substr(7);
                        for (size_t k; (k = rest.find('>')) != std::string::npos; rest.erase(0, k + 1)) chain.push_back(rest.substr(0, k));
                        chain.push_back(rest);
                        want.clear();
                    }
                    std::function<bool(const std::vector<Entry>&)> dfs = [&](const std::vector<Entry>& l) {
                        for (const Entry& x : l) {
                            if ((x.kind == Entry::Kind::Course || x.kind == Entry::Kind::Chart || x.kind == Entry::Kind::Missing) &&
                                (x.label.find(want) != std::string::npos || x.note.find(want) != std::string::npos)) {
                                chain.push_back(x.label);
                                return true;
                            }
                            if (x.kind == Entry::Kind::Folder && !x.custom) {
                                chain.push_back(x.label);
                                if (x.children.ptr() && dfs(*x.children.ptr())) return true;
                                chain.pop_back();
                            }
                        }
                        return false;
                    };
                    while (stack.size() > 1) back();
                    if (!chain.empty() || (!want.empty() && dfs(root)))
                        for (size_t ci = 0; ci < chain.size(); ++ci) {
                            const std::vector<Entry>& l = *stack.back().list;
                            for (size_t i = 0; i < l.size(); ++i)
                                if (l[i].label == chain[ci] && l[i].kind != Entry::Kind::Header) {
                                    stack.back().cursor = static_cast<int>(i), snap();
                                    if (ci + 1 < chain.size()) decide();
                                    break;
                                }
                        }
                    std::printf("[探す] %s：%zu 段\n", want.c_str(), chain.size());
                }
                else if (k == 'g') app.cycle_gauge(1);  // GAUGE を 1 つ進める
                else if (k == 'j') cycle_judge(1);      // 判定の欄を 1 つ進める
                else if (k == 'm') toggle_metronome();  // メトロノームの欄を押す
                else if (k == 'x') app.cfg.seven_key = !app.cfg.seven_key;  // 7k モードを切り替える
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
                else if (k == 'r') reload_after_play(false);  // 遊んで戻ったときの読み直し（全部の記録）
                else if (k == 'R') {
                    // 今の譜面を遊んで戻ったときと同じ読み直し
                    const Entry* cur = current();
                    if (cur && cur->kind == Entry::Kind::Chart) reload_after_play(false, {cur->chart->sha256});
                }
                else if (k == '6' || k == '9' || k == 'J' || k == 'E') {
                    // F6（スクリーンショット）・F9（保存した配置）・↓・Enter を押したことにする
                    SDL_Event ev{};
                    ev.type = SDL_EVENT_KEY_DOWN, ev.key.down = true;
                    ev.key.key = k == '6' ? SDLK_F6 : k == '9' ? SDLK_F9 : k == 'J' ? SDLK_DOWN : SDLK_RETURN;
                    SDL_PushEvent(&ev);
                }
            } else {
                if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
                quit = true;
            }
        }
        if (resort_pending) resort_pending = false, reapply_view(false);
        const uint64_t ft_present0 = SDL_GetTicksNS();
        frame_present(r);
        {
            const uint64_t end = SDL_GetTicksNS(), all = end - ft0;
            if (back_from_play) {
                back_from_play = false;
                int made = 0;
                double made_ms = 0;
                TextCache::take_made(made, made_ms);
            } else if (all > slow_ns && slow_logged < 300) {
                // 入力：イベントを片付ける（つかむ・押すの処理を含む）／更新：カーソル・一覧・パネルなど／描画：スキン／表示：画面に出す
                const uint64_t update = (ft_draw0 > ft0 + ft_events ? ft_draw0 - ft0 - ft_events : 0);
                int made = 0;
                double made_ms = 0;
                TextCache::take_made(made, made_ms);
                std::printf("遅いフレーム %.0f ms（入力 %.0f、更新 %.0f［一覧の差し替え %.0f、プレビュー %.0f、バナー %.1f、文字 %.1f］、"
                            "描画 %.0f［文字を作る %d 個 %.0f、フォルダのランプ %zu 要素 %.0f］、表示 %.0f）\n",
                            all / 1e6, ft_events / 1e6, update / 1e6, ft_swap / 1e6, ft_preview / 1e6, ft_banner / 1e6, ft_text / 1e6,
                            ft_draw / 1e6, made, made_ms, lamp_nodes, lamp_ns / 1e6, (end - ft_present0) / 1e6);
                ++slow_logged;
            } else {
                int made = 0;
                double made_ms = 0;
                TextCache::take_made(made, made_ms);  // 数を次のフレームへ持ち越さない
            }
            if (scroll_test) frame_ms.push_back(static_cast<float>(all / 1e6));
            if (open_measuring) open_max = std::max(open_max, static_cast<float>(all / 1e6));
            ft_banner = ft_text = ft_draw = ft_draw0 = 0;
            lamp_ns = 0, lamp_nodes = 0;
        }
        if (bgm_pending) {
            // 選曲画面に入るたび（遊んで戻ったときも）、BGM のセットの候補から選び直す
            bgm_pending = false;
            app.pick_bgm();
            app.mixer.play_system(kSysSelect, true);
        }
        if (app.scene_end_ns) {
            std::printf("選曲画面の最初の絵まで %.0f ms\n", (SDL_GetTicksNS() - app.scene_end_ns) / 1e6);
            app.scene_end_ns = 0;
        }
        SDL_DelayPrecise(4'000'000);
    }
    stop_preview(false);
    if (preview_load.valid()) preview_load.wait();
    for (auto& f : stale_loads) f.wait();
    app.mixer.fade_system_loop(1.0f, 0);
    app.mixer.stop_system(kSysSelect);
    {
        std::lock_guard<std::mutex> lock(banner->mu);
        banner->stop = true;
        banner->cv.notify_one();
    }
    if (banner_thread.joinable()) banner_thread.join();
    const uint64_t exit_t0 = SDL_GetTicksNS();
    {
        std::lock_guard<std::mutex> lock(prefetch->mu);
        prefetch->stop = true;
        prefetch->jobs.clear();
        prefetch->cv.notify_one();
    }
    if (prefetch_thread.joinable()) prefetch_thread.join();
    if (banner_tex) app.select_renderer->set_image_override(102, nullptr), SDL_DestroyTexture(banner_tex);
    stop_worker = true;
    if (worker.joinable()) worker.join();
    if (attr_worker.joinable()) attr_worker.join();
    std::printf("選曲画面を出る：裏の読み込みを待った %.0f ms\n", (SDL_GetTicksNS() - exit_t0) / 1e6);
}

}  // namespace rd
