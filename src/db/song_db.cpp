#include "db/song_db.h"

#include <algorithm>
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
    mtime INTEGER, size INTEGER);
CREATE INDEX IF NOT EXISTS chart_md5 ON chart(md5);
CREATE INDEX IF NOT EXISTS chart_sha256 ON chart(sha256);
CREATE TABLE IF NOT EXISTS skipped(path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER);
CREATE TABLE IF NOT EXISTS score(
    sha256 TEXT, ln_mode INTEGER, judge_profile INTEGER,
    clear INTEGER, exscore INTEGER, bp INTEGER, max_combo INTEGER,
    playcount INTEGER, clearcount INTEGER, last_played INTEGER,
    PRIMARY KEY(sha256, ln_mode, judge_profile));
)SQL";

bool is_chart_file(const fs::path& p) {
    std::string e = to_upper_ascii(p.extension().string());
    return e == ".BMS" || e == ".BME" || e == ".BML";
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
    return r;
}

const char* kChartColumns =
    "path, folder, sha256, md5, title, subtitle, artist, subartist, genre, playlevel, difficulty, notes, ln, rank, "
    "total, bpm_min, bpm_max, bpm_main, length_ms, scratch, mtime, size";

}  // namespace

bool make_chart_record(const std::string& path, ChartRecord& r) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BmsParseOptions opt;
    opt.random_seed = 1;
    BmsParseResult res = parse_bms_text(to_utf8_auto(bytes), opt);
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
    r.notes = c.judgeable_notes();
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
    return true;
}

SongDb::~SongDb() { close(); }

bool SongDb::open(const std::string& path) {
    close();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) return false;
    char* err = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "曲データベースを作れない: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

void SongDb::close() {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

ScanStats SongDb::scan(const std::vector<std::string>& dirs, int threads,
                       const std::function<void(int, int)>& progress) {
    auto t0 = std::chrono::steady_clock::now();
    ScanStats stats;
    if (!db_) return stats;

    // 1. 曲フォルダを列挙して path → (mtime, size) を作る。トップのフォルダごとに並列に回る
    std::vector<fs::path> roots;
    std::error_code ec;
    for (const std::string& d : dirs) {
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
            progress(done.load(), static_cast<int>(to_parse.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        for (auto& th : pool) th.join();
    }

    // 4. 1トランザクションで書く（prepared statement を使い回す）
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    {
        std::string sql = std::string("INSERT OR REPLACE INTO chart(") + kChartColumns +
                          ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
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
            const ChartRecord& r = records[i];
            ins.text(1, r.path).text(2, r.folder).text(3, r.sha256).text(4, r.md5).text(5, r.title).text(6, r.subtitle)
                .text(7, r.artist).text(8, r.subartist).text(9, r.genre).i64(10, r.playlevel).i64(11, r.difficulty)
                .i64(12, r.notes).i64(13, r.ln).i64(14, r.rank).i64(15, r.total).dbl(16, r.bpm_min).dbl(17, r.bpm_max)
                .dbl(18, r.bpm_main).i64(19, r.length_ms).i64(20, r.scratch).i64(21, r.mtime).i64(22, r.size);
            ins.run();
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

std::vector<ScoreRecord> SongDb::all_scores() const {
    std::vector<ScoreRecord> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT sha256, clear, exscore, bp, max_combo, playcount, clearcount, last_played FROM score "
                 "WHERE ln_mode = 0 AND judge_profile = 0");
    while (st.step()) {
        ScoreRecord r;
        r.sha256 = st.col_text(0);
        r.clear = static_cast<int>(st.col_i64(1));
        r.exscore = static_cast<int>(st.col_i64(2));
        r.bp = static_cast<int>(st.col_i64(3));
        r.max_combo = static_cast<int>(st.col_i64(4));
        r.playcount = static_cast<int>(st.col_i64(5));
        r.clearcount = static_cast<int>(st.col_i64(6));
        r.last_played = st.col_i64(7);
        out.push_back(r);
    }
    return out;
}

bool SongDb::get_score(const std::string& sha256, ScoreRecord& out) const {
    if (!db_) return false;
    Stmt st(db_, "SELECT clear, exscore, bp, max_combo, playcount, clearcount, last_played FROM score "
                 "WHERE sha256 = ? AND ln_mode = 0 AND judge_profile = 0");
    st.text(1, sha256);
    if (!st.step()) return false;
    out.sha256 = sha256;
    out.clear = static_cast<int>(st.col_i64(0));
    out.exscore = static_cast<int>(st.col_i64(1));
    out.bp = static_cast<int>(st.col_i64(2));
    out.max_combo = static_cast<int>(st.col_i64(3));
    out.playcount = static_cast<int>(st.col_i64(4));
    out.clearcount = static_cast<int>(st.col_i64(5));
    out.last_played = st.col_i64(6);
    return true;
}

void SongDb::save_score(const std::string& sha256, int clear, int exscore, int bp, int max_combo) {
    if (!db_) return;
    ScoreRecord old;
    bool had = get_score(sha256, old);
    ScoreRecord r = had ? old : ScoreRecord{};
    r.clear = std::max(r.clear, clear);
    r.exscore = std::max(r.exscore, exscore);
    r.bp = (r.bp < 0 || !had) ? bp : std::min(r.bp, bp);
    r.max_combo = std::max(r.max_combo, max_combo);
    r.playcount++;
    if (clear >= 2) r.clearcount++;
    r.last_played = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Stmt st(db_, "INSERT OR REPLACE INTO score(sha256, ln_mode, judge_profile, clear, exscore, bp, max_combo, "
                 "playcount, clearcount, last_played) VALUES(?,0,0,?,?,?,?,?,?,?)");
    st.text(1, sha256).i64(2, r.clear).i64(3, r.exscore).i64(4, r.bp).i64(5, r.max_combo).i64(6, r.playcount)
        .i64(7, r.clearcount).i64(8, r.last_played);
    st.run();
}

}  // namespace bmj
