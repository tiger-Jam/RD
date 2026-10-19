#include "db/song_db.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "chart/bms_parser.h"
#include "chart/chart_loader.h"
#include "sqlite3.h"
#include "util/hash.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS chart(
    path TEXT PRIMARY KEY, folder TEXT, sha256 TEXT, md5 TEXT,
    title TEXT, subtitle TEXT, artist TEXT, subartist TEXT, genre TEXT,
    playlevel INTEGER, difficulty INTEGER, notes INTEGER, ln INTEGER, rank INTEGER, total INTEGER,
    bpm_min REAL, bpm_max REAL, bpm_main REAL, length_ms INTEGER, scratch INTEGER,
    mtime INTEGER, size INTEGER, banner TEXT, stagefile TEXT, backbmp TEXT, ln_free INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS chart_md5 ON chart(md5);
CREATE INDEX IF NOT EXISTS chart_sha256 ON chart(sha256);
CREATE TABLE IF NOT EXISTS skipped(path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER);
CREATE TABLE IF NOT EXISTS lr2_score(
    md5 TEXT PRIMARY KEY, clear INTEGER, exscore INTEGER, bp INTEGER, max_combo INTEGER,
    playcount INTEGER, clearcount INTEGER,
    pg INTEGER DEFAULT 0, gr INTEGER DEFAULT 0, gd INTEGER DEFAULT 0, bd INTEGER DEFAULT 0, pr INTEGER DEFAULT 0,
    op_history INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS score(
    sha256 TEXT, ln_mode INTEGER, judge_profile INTEGER,
    clear INTEGER, exscore INTEGER, bp INTEGER, max_combo INTEGER,
    playcount INTEGER, clearcount INTEGER, last_played INTEGER,
    pg INTEGER DEFAULT 0, gr INTEGER DEFAULT 0, gd INTEGER DEFAULT 0, bd INTEGER DEFAULT 0, pr INTEGER DEFAULT 0,
    op_history INTEGER DEFAULT 0,
    PRIMARY KEY(sha256, ln_mode, judge_profile));
CREATE TABLE IF NOT EXISTS player(
    id INTEGER PRIMARY KEY, playcount INTEGER, clear INTEGER, fail INTEGER,
    pg INTEGER, gr INTEGER, gd INTEGER, bd INTEGER, pr INTEGER);
)SQL";

bool is_chart_file(const fs::path& p) {
    std::string e = to_upper_ascii(p.extension().string());
    return e == ".BMS" || e == ".BME" || e == ".BML" || e == ".OSU" || e == ".BMSON";
}

int64_t mtime_of(const fs::directory_entry& e) {
    std::error_code ec;
    auto t = e.last_write_time(ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& text(int i, const std::string& v) {
        sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& i64(int i, int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    Stmt& dbl(int i, double v) {
        sqlite3_bind_double(s, i, v);
        return *this;
    }
    bool step() { return sqlite3_step(s) == SQLITE_ROW; }
    void reset() {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    void run() {
        sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    std::string col_text(int i) const {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    int64_t col_i64(int i) const { return sqlite3_column_int64(s, i); }
    double col_dbl(int i) const { return sqlite3_column_double(s, i); }
};

ChartRecord read_chart_row(const Stmt& st) {
    ChartRecord r;
    r.path = st.col_text(0);
    r.folder = st.col_text(1);
    r.sha256 = st.col_text(2);
    r.md5 = st.col_text(3);
    r.title = st.col_text(4);
    r.subtitle = st.col_text(5);
    r.artist = st.col_text(6);
    r.subartist = st.col_text(7);
    r.genre = st.col_text(8);
    r.playlevel = static_cast<int>(st.col_i64(9));
    r.difficulty = static_cast<int>(st.col_i64(10));
    r.notes = static_cast<int>(st.col_i64(11));
    r.ln = static_cast<int>(st.col_i64(12));
    r.rank = static_cast<int>(st.col_i64(13));
    r.total = static_cast<int>(st.col_i64(14));
    r.bpm_min = st.col_dbl(15);
    r.bpm_max = st.col_dbl(16);
    r.bpm_main = st.col_dbl(17);
    r.length_ms = static_cast<int>(st.col_i64(18));
    r.scratch = st.col_i64(19) != 0;
    r.mtime = st.col_i64(20);
    r.size = st.col_i64(21);
    r.banner = st.col_text(22);
    r.stagefile = st.col_text(23);
    r.backbmp = st.col_text(24);
    r.ln_free = static_cast<int>(st.col_i64(25));
    return r;
}

// 仮の記録（LR2 から取り込んで、まだ譜面を読んでいないもの）の sha256 の欄
constexpr const char* kProvisional = "md5:";

const char* kChartColumns =
    "path, folder, sha256, md5, title, subtitle, artist, subartist, genre, playlevel, difficulty, notes, ln, rank, "
    "total, bpm_min, bpm_max, bpm_main, length_ms, scratch, mtime, size, banner, stagefile, backbmp, ln_free";

}  // namespace

namespace {
// #DIFFICULTY の無い譜面の難易度を、#TITLE から推定する（LR2: fileutil:287）。分からなければ 0
int difficulty_from_title(const std::string& title) {
    std::string up = title;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto ends = [&](const std::string& t) { return up.size() >= t.size() && up.compare(up.size() - t.size(), t.size(), t) == 0; };
    int d = 0;
    if (ends("HARD")) d = 2;
    if (ends("HYPER")) d = 3;
    if (ends("ANOTHER")) d = 4;
    if (ends("EASY")) d = 1;
    if (ends("EX")) d = 4;
    if (ends("MANIAC")) d = 4;
    // 末尾の括弧の中の言葉
    static const std::pair<const char*, const char*> kTokens[] = {
        {"(", ")"}, {"[", "]"}, {"-", "-"}, {"\"", "\""}, {"<", ">"}, {"～", "～"}, {"【", "】"}};
    for (auto [l, r] : kTokens) {
        std::string rs = r;
        if (!ends(rs) || up.size() <= rs.size()) continue;
        size_t pos = up.rfind(l, up.size() - rs.size() - 1);
        if (pos == std::string::npos || pos == 0) continue;
        std::string in = up.substr(pos + std::strlen(l));
        static const std::pair<const char*, int> kWords[] = {
            {"BEG", 1}, {"HARD", 3}, {"HYPE", 3}, {"HD", 3}, {"5H", 3}, {"7H", 3}, {"10H", 3}, {"14H", 3}, {"9H", 3},
            {"DIF", 3}, {"VERYHARD", 4}, {"EX", 4}, {"AN", 4}, {"SHD", 4}, {"5A", 4}, {"7A", 4}, {"10A", 4}, {"14A", 4},
            {"9A", 4}, {"ULT", 4}, {"MANI", 4}, {"LUNA", 4}, {"AHO", 4}, {"AFO", 4}, {"ASDF", 4}, {"HELL", 4}};
        for (auto [w, v] : kWords)
            if (in.find(w) != std::string::npos) {
                d = v;
                break;
            }
        break;
    }
    return d;
}
}  // namespace

bool make_chart_record(const std::string& path, ChartRecord& r) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BmsParseOptions opt;
    opt.random_seed = 1;
    BmsParseResult res = parse_chart_bytes(path, bytes, opt);
    if (!res.ok) return false;
    const Chart& c = res.chart;
    r.path = path;
    r.folder = fs::path(path).parent_path().string();
    r.md5 = md5_hex(bytes);
    r.sha256 = sha256_hex(bytes);
    r.title = c.meta.title;
    r.subtitle = c.meta.subtitle;
    r.artist = c.meta.artist;
    r.subartist = c.meta.subartist;
    r.genre = c.meta.genre;
    r.playlevel = c.meta.playlevel;
    r.difficulty = c.meta.difficulty;
    if (r.difficulty <= 0 || r.difficulty > 5) r.difficulty = difficulty_from_title(c.meta.title);
    r.notes = c.judgeable_notes(0);
    r.ln_free = c.free_ln_count();
    r.ln = static_cast<int>(std::count_if(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.kind == NoteKind::Long; }));
    r.rank = c.judge_rank;
    r.total = static_cast<int>(c.total);
    r.bpm_min = 1e9;
    r.bpm_max = 0;
    for (const ScrollSeg& s : c.scroll) r.bpm_min = std::min(r.bpm_min, s.bpm), r.bpm_max = std::max(r.bpm_max, s.bpm);
    if (c.scroll.empty()) r.bpm_min = r.bpm_max = c.initial_bpm;
    r.bpm_main = c.main_bpm;
    r.length_ms = static_cast<int>(c.end_us / 1000);
    r.scratch = c.layout == KeyLayout::K7S;
    r.banner = c.meta.banner;
    r.stagefile = c.meta.stagefile;
    r.backbmp = c.meta.backbmp;
    return true;
}

SongDb::~SongDb() { close(); }

bool SongDb::clear_charts() {
    if (!db_) return false;
    return sqlite3_exec(db_, "DELETE FROM chart; DELETE FROM skipped;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

void SongDb::set_chart_tag(const std::string& path, int difficulty, int playlevel) {
    if (!db_) return;
    Stmt st(db_, "UPDATE chart SET difficulty = ?, playlevel = ? WHERE path = ?");
    st.i64(1, difficulty).i64(2, playlevel).text(3, path).run();
}

bool SongDb::open(const std::string& path) {
    close();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) return false;
    // 選曲画面の裏の読み直しと、プレイ後のスコアの記録が重なっても待つ
    sqlite3_busy_timeout(db_, 5000);
    char* err = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "曲データベースを作れない: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    // 古いデータベースに画像の欄を足す。足したら、次の読み直しで全部の譜面を読み直させる
    bool has_banner = false;
    {
        Stmt info(db_, "PRAGMA table_info(chart)");
        while (info.step())
            if (info.col_text(1) == "banner") has_banner = true;
    }
    // 古いデータベースに、判定の内訳とオプションの履歴の欄を足す
    for (const char* table : {"score", "lr2_score"}) {
        bool has = false;
        Stmt info(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
        while (info.step())
            if (info.col_text(1) == "op_history") has = true;
        if (!has) {
            std::string t = table;
            std::string sql;
            for (const char* col : {"pg", "gr", "gd", "bd", "pr", "op_history"})
                sql += "ALTER TABLE " + t + " ADD COLUMN " + col + " INTEGER DEFAULT 0;";
            sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
        }
    }
    // 古いデータベースに、種類を決めていない LN の数の欄を足す（足したら全部読み直させる）
    {
        bool has_ln_free = false;
        Stmt info(db_, "PRAGMA table_info(chart)");
        while (info.step())
            if (info.col_text(1) == "ln_free") has_ln_free = true;
        if (!has_ln_free && has_banner)
            sqlite3_exec(db_, "ALTER TABLE chart ADD COLUMN ln_free INTEGER DEFAULT 0; UPDATE chart SET mtime = 0;", nullptr, nullptr, nullptr);
    }
    if (!has_banner) {
        sqlite3_exec(db_,
                     "ALTER TABLE chart ADD COLUMN banner TEXT; ALTER TABLE chart ADD COLUMN stagefile TEXT; "
                     "ALTER TABLE chart ADD COLUMN backbmp TEXT; ALTER TABLE chart ADD COLUMN ln_free INTEGER DEFAULT 0; "
                     "UPDATE chart SET mtime = 0;",
                     nullptr, nullptr, nullptr);
    }
    return true;
}

void SongDb::close() {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

namespace {
// 譜面の記録を 1 件書く（ins は INSERT OR REPLACE INTO chart の文）
void put_chart(Stmt& ins, const ChartRecord& r) {
    ins.text(1, r.path).text(2, r.folder).text(3, r.sha256).text(4, r.md5).text(5, r.title).text(6, r.subtitle)
        .text(7, r.artist).text(8, r.subartist).text(9, r.genre).i64(10, r.playlevel).i64(11, r.difficulty)
        .i64(12, r.notes).i64(13, r.ln).i64(14, r.rank).i64(15, r.total).dbl(16, r.bpm_min).dbl(17, r.bpm_max)
        .dbl(18, r.bpm_main).i64(19, r.length_ms).i64(20, r.scratch).i64(21, r.mtime).i64(22, r.size)
        .text(23, r.banner).text(24, r.stagefile).text(25, r.backbmp).i64(26, r.ln_free);
    ins.run();
}
std::string insert_chart_sql() {
    return std::string("INSERT OR REPLACE INTO chart(") + kChartColumns + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
}
}  // namespace

ScanStats SongDb::scan(const std::vector<std::string>& dirs, int threads,
                       const std::function<void(int, int, const std::string&)>& progress, const std::string& only_under) {
    auto t0 = std::chrono::steady_clock::now();
    ScanStats stats;
    if (!db_) return stats;

    // 1. 曲フォルダを列挙して path → (mtime, size) を作る。トップのフォルダごとに並列に回る
    std::vector<fs::path> roots;
    std::error_code ec;
    for (const std::string& d : only_under.empty() ? dirs : std::vector<std::string>{only_under}) {
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (e.is_directory(ec)) roots.push_back(e.path());
            else if (is_chart_file(e.path())) roots.push_back(e.path());
        }
    }
    struct FileInfo {
        int64_t mtime, size;
    };
    std::unordered_map<std::string, FileInfo> on_disk;
    std::mutex mu;
    std::atomic<size_t> next_root{0};
    auto walker = [&] {
        std::vector<std::pair<std::string, FileInfo>> local;
        for (;;) {
            size_t i = next_root.fetch_add(1);
            if (i >= roots.size()) break;
            std::error_code ec2;
            if (fs::is_regular_file(roots[i], ec2)) {
                fs::directory_entry e(roots[i], ec2);
                local.push_back({roots[i].string(), {mtime_of(e), static_cast<int64_t>(e.file_size(ec2))}});
                continue;
            }
            for (auto it = fs::recursive_directory_iterator(roots[i], fs::directory_options::skip_permission_denied, ec2);
                 !ec2 && it != fs::recursive_directory_iterator(); it.increment(ec2)) {
                if (!it->is_regular_file(ec2) || !is_chart_file(it->path())) continue;
                local.push_back({it->path().string(), {mtime_of(*it), static_cast<int64_t>(it->file_size(ec2))}});
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        for (auto& [p, fi] : local) on_disk.emplace(std::move(p), fi);
    };
    int nthreads = std::max(1, threads);
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < nthreads; ++t) pool.emplace_back(walker);
        for (auto& th : pool) th.join();
    }
    stats.found = static_cast<int>(on_disk.size());

    // 2. データベースの状態と比べて、追加・変更・削除を分ける
    std::unordered_map<std::string, FileInfo> in_db;
    {
        Stmt st(db_, "SELECT path, mtime, size FROM chart");
        while (st.step()) in_db.emplace(st.col_text(0), FileInfo{st.col_i64(1), st.col_i64(2)});
        // 読めなかった譜面（DPなど）も覚えておき、変わっていなければ読み直さない
        Stmt sk(db_, "SELECT path, mtime, size FROM skipped");
        while (sk.step()) in_db.emplace(sk.col_text(0), FileInfo{sk.col_i64(1), sk.col_i64(2)});
    }
    if (!only_under.empty()) {
        // そのフォルダの下の譜面だけを比べる（区切りと大文字小文字の違いは無視する）
        auto key = [](const std::string& p) {
            std::string k = fs::path(p).lexically_normal().generic_string();
            for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return k;
        };
        std::string prefix = key(only_under);
        if (prefix.empty() || prefix.back() != '/') prefix += '/';
        for (auto it = in_db.begin(); it != in_db.end();)
            it = key(it->first).rfind(prefix, 0) == 0 ? std::next(it) : in_db.erase(it);
    }
    std::vector<std::string> to_parse, to_remove;
    for (auto& [p, fi] : on_disk) {
        auto it = in_db.find(p);
        if (it == in_db.end()) to_parse.push_back(p), stats.added++;
        else if (it->second.mtime != fi.mtime || it->second.size != fi.size) to_parse.push_back(p), stats.updated++;
    }
    for (auto& [p, fi] : in_db)
        if (!on_disk.count(p)) to_remove.push_back(p);
    stats.removed = static_cast<int>(to_remove.size());

    // 3. 変わったファイルだけを並列にパースする
    std::vector<ChartRecord> records(to_parse.size());
    std::vector<char> ok(to_parse.size(), 0);
    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    auto parser = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= to_parse.size()) break;
            ok[i] = make_chart_record(to_parse[i], records[i]);
            if (ok[i]) {
                const FileInfo& fi = on_disk[to_parse[i]];
                records[i].mtime = fi.mtime;
                records[i].size = fi.size;
            }
            done.fetch_add(1);
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < nthreads; ++t) pool.emplace_back(parser);
        while (progress && done.load() < static_cast<int>(to_parse.size())) {
            size_t cur = std::min(next.load(), to_parse.size());
            progress(done.load(), static_cast<int>(to_parse.size()), cur ? to_parse[cur - 1] : std::string());
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        for (auto& th : pool) th.join();
    }

    // 4. 1トランザクションで書く（prepared statement を使い回す）
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    {
        std::string sql = insert_chart_sql();
        Stmt ins(db_, sql.c_str());
        Stmt del(db_, "DELETE FROM chart WHERE path = ?");
        Stmt skip(db_, "INSERT OR REPLACE INTO skipped(path, mtime, size) VALUES(?,?,?)");
        Stmt unskip(db_, "DELETE FROM skipped WHERE path = ?");
        for (size_t i = 0; i < records.size(); ++i) {
            if (!ok[i]) {
                // 読めない譜面（DPなど）は一覧に出さない
                del.text(1, to_parse[i]).run();
                const FileInfo& fi = on_disk[to_parse[i]];
                skip.text(1, to_parse[i]).i64(2, fi.mtime).i64(3, fi.size).run();
                stats.failed++;
                continue;
            }
            unskip.text(1, to_parse[i]).run();
            put_chart(ins, records[i]);
        }
        for (const std::string& p : to_remove) del.text(1, p).run(), unskip.text(1, p).run();
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return stats;
}

std::vector<ChartRecord> SongDb::all_charts() const {
    std::vector<ChartRecord> out;
    if (!db_) return out;
    std::string sql = std::string("SELECT ") + kChartColumns + " FROM chart";
    Stmt st(db_, sql.c_str());
    while (st.step()) out.push_back(read_chart_row(st));
    return out;
}

bool SongDb::find_by_md5(const std::string& md5, ChartRecord& out) const {
    if (!db_) return false;
    std::string sql = std::string("SELECT ") + kChartColumns + " FROM chart WHERE md5 = ? LIMIT 1";
    Stmt st(db_, sql.c_str());
    st.text(1, md5);
    if (!st.step()) return false;
    out = read_chart_row(st);
    return true;
}

namespace {

// BMjoyの記録とLR2から取り込んだ記録を合わせる（ランプ・EX・コンボは高いほう、BPは低いほう、回数は足す。
// 判定の内訳は EX の高いほうのもの、オプションの履歴は両方）
void merge_score(ScoreRecord& r, int clear, int ex, int bp, int combo, int play, int clr, const Stmt& st, int jcol) {
    if (ex > r.exscore || (r.exscore == 0 && ex == 0))
        for (int i = 0; i < 5; ++i) r.judge[i] = static_cast<int>(st.col_i64(jcol + i));
    r.op_history |= static_cast<uint32_t>(st.col_i64(jcol + 5));
    r.clear = std::max(r.clear, clear);
    r.exscore = std::max(r.exscore, ex);
    if (bp >= 0) r.bp = r.bp < 0 ? bp : std::min(r.bp, bp);
    r.max_combo = std::max(r.max_combo, combo);
    r.playcount += play;
    r.clearcount += clr;
}

// 譜面ごとの記録。種類を決めていない LN のある譜面は ln_mode の記録、それ以外は 0 の記録を読む。
// 曲データベースに無いキー（コース）の記録と、LR2 から取り込んだ記録（LN で遊んだもの）も合わせる
std::string merged_scores_sql(int ln_mode) {
    std::string m = std::to_string(ln_mode);
    return R"SQL(
SELECT c.sha256, s.clear, s.exscore, s.bp, s.max_combo, s.playcount, s.clearcount, s.last_played, 0,
       s.pg, s.gr, s.gd, s.bd, s.pr, s.op_history
  FROM score s JOIN (SELECT sha256, MAX(ln_free) AS ln_free FROM chart GROUP BY sha256) c ON c.sha256 = s.sha256
 WHERE s.judge_profile = 0 AND s.ln_mode = (CASE WHEN c.ln_free > 0 THEN )SQL" + m + R"SQL( ELSE 0 END)
UNION ALL
SELECT s.sha256, s.clear, s.exscore, s.bp, s.max_combo, s.playcount, s.clearcount, s.last_played, 0,
       s.pg, s.gr, s.gd, s.bd, s.pr, s.op_history
  FROM score s
 WHERE s.judge_profile = 0 AND s.ln_mode = 0 AND s.sha256 NOT IN (SELECT sha256 FROM chart)
UNION ALL
SELECT c.sha256, l.clear, l.exscore, l.bp, l.max_combo, l.playcount, l.clearcount, 0, 1,
       l.pg, l.gr, l.gd, l.bd, l.pr, l.op_history
  FROM lr2_score l JOIN (SELECT sha256, md5, MAX(ln_free) AS ln_free FROM chart GROUP BY sha256, md5) c ON c.md5 = l.md5
 WHERE c.ln_free = 0 OR )SQL" + m + " = 0\n";
}

}  // namespace

std::vector<ScoreRecord> SongDb::all_scores(int ln_mode) const {
    std::vector<ScoreRecord> out;
    if (!db_) return out;
    std::unordered_map<std::string, size_t> idx;
    std::string sql = merged_scores_sql(ln_mode);
    Stmt st(db_, sql.c_str());
    while (st.step()) {
        std::string sha = st.col_text(0);
        auto it = idx.find(sha);
        if (it == idx.end()) {
            ScoreRecord r;
            r.sha256 = sha;
            r.bp = -1;
            it = idx.emplace(sha, out.size()).first;
            out.push_back(r);
        }
        ScoreRecord& r = out[it->second];
        merge_score(r, static_cast<int>(st.col_i64(1)), static_cast<int>(st.col_i64(2)), static_cast<int>(st.col_i64(3)),
                    static_cast<int>(st.col_i64(4)), static_cast<int>(st.col_i64(5)), static_cast<int>(st.col_i64(6)), st, 9);
        r.last_played = std::max(r.last_played, st.col_i64(7));
    }
    return out;
}

bool SongDb::get_score(const std::string& sha256, ScoreRecord& out, int ln_mode) const {
    if (!db_) return false;
    std::string sql = "SELECT * FROM (" + merged_scores_sql(ln_mode) + ") WHERE sha256 = ?";
    Stmt st(db_, sql.c_str());
    st.text(1, sha256);
    bool found = false;
    out = ScoreRecord{};
    out.sha256 = sha256;
    out.bp = -1;
    while (st.step()) {
        found = true;
        merge_score(out, static_cast<int>(st.col_i64(1)), static_cast<int>(st.col_i64(2)), static_cast<int>(st.col_i64(3)),
                    static_cast<int>(st.col_i64(4)), static_cast<int>(st.col_i64(5)), static_cast<int>(st.col_i64(6)), st, 9);
        out.last_played = std::max(out.last_played, st.col_i64(7));
    }
    return found;
}

int SongDb::import_lr2_scores(const std::string& lr2_score_db, std::string* error) {
    if (!db_) return -1;
    sqlite3* src = nullptr;
    if (sqlite3_open_v2(lr2_score_db.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error) *error = "開けない: " + lr2_score_db;
        sqlite3_close(src);
        return -1;
    }
    int count = 0;
    {
        // LR2の score 表（LR2_songmanage.cpp:2696）。EXスコアは PGREAT×2 + GREAT
        // 判定の内訳とオプションの履歴もあれば取り込む（無い版もあるので、無ければ 0）
        Stmt full(src, "SELECT hash, clear, perfect, great, minbp, maxcombo, playcount, clearcount, good, bad, poor, op_history "
                       "FROM score WHERE length(hash) = 32");
        Stmt basic(src, full.s ? "SELECT 1" : "SELECT hash, clear, perfect, great, minbp, maxcombo, playcount, clearcount, "
                                              "0, 0, 0, 0 FROM score WHERE length(hash) = 32");
        Stmt& rd = full.s ? full : basic;
        if (!rd.s) {
            if (error) *error = "LR2のスコアデータベースではない: " + lr2_score_db;
            sqlite3_close(src);
            return -1;
        }
        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        Stmt wr(db_, "INSERT OR REPLACE INTO lr2_score(md5, clear, exscore, bp, max_combo, playcount, clearcount, "
                     "pg, gr, gd, bd, pr, op_history) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
        while (rd.step()) {
            std::string md5 = rd.col_text(0);
            for (char& c : md5) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int ex = static_cast<int>(rd.col_i64(2) * 2 + rd.col_i64(3));
            wr.text(1, md5).i64(2, rd.col_i64(1)).i64(3, ex).i64(4, rd.col_i64(4)).i64(5, rd.col_i64(5))
                .i64(6, rd.col_i64(6)).i64(7, rd.col_i64(7)).i64(8, rd.col_i64(2)).i64(9, rd.col_i64(3)).i64(10, rd.col_i64(8))
                .i64(11, rd.col_i64(9)).i64(12, rd.col_i64(10)).i64(13, rd.col_i64(11));
            wr.run();
            ++count;
        }
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }
    sqlite3_close(src);
    return count;
}

void SongDb::save_score(const std::string& sha256, int clear, int exscore, int bp, int max_combo) {
    PlayRecord r;
    r.clear = clear, r.exscore = exscore, r.bp = bp, r.max_combo = max_combo;
    save_score(sha256, r);
}

PlayerStats SongDb::player_stats() const {
    PlayerStats p;
    if (!db_) return p;
    Stmt st(db_, "SELECT playcount, clear, fail, pg, gr, gd, bd, pr FROM player WHERE id = 0");
    if (st.step()) {
        p.playcount = static_cast<int>(st.col_i64(0)), p.clear = static_cast<int>(st.col_i64(1)), p.fail = static_cast<int>(st.col_i64(2));
        for (int i = 0; i < 5; ++i) p.judge[i] = st.col_i64(3 + i);
    }
    return p;
}

void SongDb::save_score(const std::string& sha256, const PlayRecord& play) {
    if (!db_) return;
    const int clear = play.clear, exscore = play.exscore, bp = play.bp, max_combo = play.max_combo;
    ScoreRecord r;
    bool had = false;
    {
        Stmt st(db_, "SELECT clear, exscore, bp, max_combo, playcount, clearcount, pg, gr, gd, bd, pr, op_history FROM score "
                     "WHERE sha256 = ? AND ln_mode = ? AND judge_profile = 0");
        st.text(1, sha256).i64(2, play.ln_mode);
        if (st.step()) {
            had = true;
            r.clear = static_cast<int>(st.col_i64(0));
            r.exscore = static_cast<int>(st.col_i64(1));
            r.bp = static_cast<int>(st.col_i64(2));
            r.max_combo = static_cast<int>(st.col_i64(3));
            r.playcount = static_cast<int>(st.col_i64(4));
            r.clearcount = static_cast<int>(st.col_i64(5));
            for (int i = 0; i < 5; ++i) r.judge[i] = static_cast<int>(st.col_i64(6 + i));
            r.op_history = static_cast<uint32_t>(st.col_i64(11));
        }
    }
    r.op_history |= play.op_history;
    if (!play.course_stage || clear >= 5) r.clear = std::max(r.clear, clear);
    if (!play.lamp_only) {
        // 判定の内訳は、EX スコアを更新したとき（初めてのときも）だけ入れ替える
        if (!had || exscore > r.exscore) r.judge = play.judge;
        r.exscore = std::max(r.exscore, exscore);
        r.bp = (r.bp < 0 || !had) ? bp : std::min(r.bp, bp);
        r.max_combo = std::max(r.max_combo, max_combo);
    }
    if (!play.course_stage) {
        r.playcount++;
        if (clear >= 2) r.clearcount++;
    }
    r.last_played = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Stmt st(db_, "INSERT OR REPLACE INTO score(sha256, ln_mode, judge_profile, clear, exscore, bp, max_combo, "
                 "playcount, clearcount, last_played, pg, gr, gd, bd, pr, op_history) VALUES(?,?,0,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    st.text(1, sha256).i64(2, play.ln_mode).i64(3, r.clear).i64(4, r.exscore).i64(5, r.bp).i64(6, r.max_combo)
        .i64(7, r.playcount).i64(8, r.clearcount).i64(9, r.last_played).i64(10, r.judge[0]).i64(11, r.judge[1])
        .i64(12, r.judge[2]).i64(13, r.judge[3]).i64(14, r.judge[4]).i64(15, r.op_history);
    st.run();
    if (play.course_stage) return;
    // プレイヤーの累計
    PlayerStats p = player_stats();
    p.playcount++;
    (clear >= 2 ? p.clear : p.fail)++;
    for (int i = 0; i < 5; ++i) p.judge[i] += play.judge[i];
    Stmt ps(db_, "INSERT OR REPLACE INTO player(id, playcount, clear, fail, pg, gr, gd, bd, pr) VALUES(0,?,?,?,?,?,?,?,?)");
    ps.i64(1, p.playcount).i64(2, p.clear).i64(3, p.fail);
    for (int i = 0; i < 5; ++i) ps.i64(4 + i, p.judge[i]);
    ps.run();
}

int SongDb::import_lr2_songs(const std::string& lr2_song_db, std::string* error,
                             const std::function<void(int, int)>& progress, const std::vector<std::string>& roots) {
    auto upper = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    };
    std::vector<std::string> root_keys;
    for (const std::string& r : roots) {
        std::string k = upper(fs::path(r).lexically_normal().string());
        while (!k.empty() && (k.back() == '/' || k.back() == '\\')) k.pop_back();
        root_keys.push_back(k);
    }
    auto under_root = [&](const std::string& p) {
        if (root_keys.empty()) return true;
        std::string up = upper(p);
        for (const std::string& k : root_keys)
            if (up.size() > k.size() && up.compare(0, k.size(), k) == 0 && (up[k.size()] == '/' || up[k.size()] == '\\')) return true;
        return false;
    };
    if (!db_) return -1;
    sqlite3* src = nullptr;
    if (sqlite3_open_v2(lr2_song_db.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error) *error = "開けない: " + lr2_song_db;
        sqlite3_close(src);
        return -1;
    }
    struct Row {
        ChartRecord r;
        int mode = 0;
        bool exists = false;
    };
    std::vector<Row> rows;
    {
        Stmt st(src, "SELECT path, hash, title, subtitle, genre, artist, subartist, level, difficulty, maxbpm, minbpm, mode, "
                     "judge, longnote, karinotes, stagefile, banner, backbmp FROM song");
        if (!st.s) {
            if (error) *error = "LR2 の曲データベースではない: " + lr2_song_db;
            sqlite3_close(src);
            return -1;
        }
        // 文字は UTF-8 のものと Shift_JIS のものがある（ツールで直した LR2 の DB がある）ので、見分けて UTF-8 にそろえる
        auto text = [&](int col) { return to_utf8_auto(st.col_text(col)); };
        while (st.step()) {
            Row w;
            ChartRecord& r = w.r;
            r.path = text(0);
            if (!under_root(r.path)) continue;
            r.folder = fs::path(r.path).parent_path().string();
            r.md5 = st.col_text(1);
            for (char& ch : r.md5) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            r.sha256 = std::string(kProvisional) + r.md5;
            r.title = text(2), r.subtitle = text(3), r.genre = text(4), r.artist = text(5), r.subartist = text(6);
            r.playlevel = static_cast<int>(st.col_i64(7));
            r.difficulty = static_cast<int>(st.col_i64(8));
            r.bpm_max = st.col_dbl(9), r.bpm_min = st.col_dbl(10), r.bpm_main = r.bpm_max;
            w.mode = static_cast<int>(st.col_i64(11));
            r.rank = static_cast<int>(st.col_i64(12));
            r.ln = static_cast<int>(st.col_i64(13));
            r.ln_free = r.ln;  // LR2 の譜面は種類を決めていない LN として扱う（読み直せば正しい数になる）
            r.notes = static_cast<int>(st.col_i64(14));
            r.stagefile = text(15), r.banner = text(16), r.backbmp = text(17);
            rows.push_back(std::move(w));
        }
    }
    sqlite3_close(src);
    // 更新日時と大きさを並列に読む（ファイルの中身は開かない）
    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    auto worker = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= rows.size()) return;
            std::error_code ec;
            fs::directory_entry e(fs::path(rows[i].r.path), ec);
            if (!ec && e.is_regular_file(ec)) {
                rows[i].exists = true;
                rows[i].r.mtime = mtime_of(e);
                rows[i].r.size = static_cast<int64_t>(e.file_size(ec));
            }
            done.fetch_add(1);
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 8; ++t) pool.emplace_back(worker);
        while (progress && done.load() < static_cast<int>(rows.size())) {
            progress(done.load(), static_cast<int>(rows.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        for (auto& th : pool) th.join();
    }
    int imported = 0;
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    {
        std::string sql = insert_chart_sql();
        Stmt ins(db_, sql.c_str());
        Stmt skip(db_, "INSERT OR REPLACE INTO skipped(path, mtime, size) VALUES(?,?,?)");
        Stmt has(db_, "SELECT 1 FROM chart WHERE path = ? AND sha256 NOT LIKE 'md5:%'");
        for (const Row& w : rows) {
            if (!w.exists) continue;
            if (w.mode != 7) {
                // 7 鍵以外（BMjoy では遊べない譜面）は、読み直さないように覚えておく
                skip.text(1, w.r.path).i64(2, w.r.mtime).i64(3, w.r.size).run();
                continue;
            }
            // すでに BMjoy が読んだ譜面は、そのまま残す
            has.text(1, w.r.path);
            bool already = has.step();
            has.reset();
            if (already) continue;
            put_chart(ins, w.r);
            imported++;
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return imported;
}

int SongDb::provisional_count() const {
    if (!db_) return 0;
    Stmt st(db_, "SELECT COUNT(*) FROM chart WHERE sha256 LIKE 'md5:%'");
    return st.step() ? static_cast<int>(st.col_i64(0)) : 0;
}

int SongDb::fill_provisional(int limit, int threads) {
    if (!db_) return 0;
    std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>> paths;
    {
        Stmt st(db_, "SELECT path, mtime, size FROM chart WHERE sha256 LIKE 'md5:%' LIMIT ?");
        st.i64(1, limit);
        while (st.step()) paths.push_back({st.col_text(0), {st.col_i64(1), st.col_i64(2)}});
    }
    std::vector<ChartRecord> recs(paths.size());
    std::vector<char> ok(paths.size(), 0);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= paths.size()) return;
            ok[i] = make_chart_record(paths[i].first, recs[i]);
            recs[i].mtime = paths[i].second.first;
            recs[i].size = paths[i].second.second;
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < std::max(1, threads); ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
    }
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    {
        std::string sql = insert_chart_sql();
        Stmt ins(db_, sql.c_str());
        Stmt del(db_, "DELETE FROM chart WHERE path = ?");
        Stmt skip(db_, "INSERT OR REPLACE INTO skipped(path, mtime, size) VALUES(?,?,?)");
        for (size_t i = 0; i < paths.size(); ++i) {
            if (ok[i]) {
                put_chart(ins, recs[i]);
            } else {
                // 読めない譜面は一覧から外し、変わるまで読み直さない
                del.text(1, paths[i].first).run();
                skip.text(1, paths[i].first).i64(2, paths[i].second.first).i64(3, paths[i].second.second).run();
            }
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return static_cast<int>(paths.size());
}

bool SongDb::refresh_chart(const std::string& path) {
    if (!db_) return false;
    ChartRecord r;
    std::error_code ec;
    fs::directory_entry e(fs::path(path), ec);
    if (ec || !make_chart_record(path, r)) return false;
    r.mtime = mtime_of(e);
    r.size = static_cast<int64_t>(e.file_size(ec));
    std::string sql = insert_chart_sql();
    Stmt ins(db_, sql.c_str());
    put_chart(ins, r);
    return true;
}

std::vector<std::string> SongDb::lr2_table_urls(const std::string& lr2_song_db) {
    std::vector<std::string> urls;
    sqlite3* src = nullptr;
    if (sqlite3_open_v2(lr2_song_db.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        Stmt st(src, "SELECT page_url FROM playlist WHERE page_url IS NOT NULL AND page_url != '' ORDER BY playlist_id");
        while (st.s && st.step()) urls.push_back(st.col_text(0));
    }
    sqlite3_close(src);
    return urls;
}

}  // namespace bmj
