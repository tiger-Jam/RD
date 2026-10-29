#include "db/lr2_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

#include "sqlite3.h"
#include "util/data_dir.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

// LR2 のフォルダの番号の CRC32。0x80 以上のバイトは、符号付きの文字として 32 ビットに広げてから混ぜる（LR2 の値と合わせるため。
// 手元の LR2 の song.db で、日本語のフォルダの番号がこれで合う）
uint32_t lr2_crc32(const std::string& bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8) ^ (b >= 0x80 ? 0x00FFFFFFu : 0u);
    return c ^ 0xFFFFFFFFu;
}

// LR2 の形のフォルダのパス（区切りは逆スラッシュで、末尾にも付ける）
std::string lr2_dir(std::string d) {
    std::replace(d.begin(), d.end(), '/', '\\');
    if (d.empty() || d.back() != '\\') d += '\\';
    return d;
}

std::string lr2_file(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

// 親のフォルダ（末尾の \ 付き。無ければ空）
std::string parent_dir(const std::string& dir) {
    if (dir.size() < 2) return {};
    const size_t k = dir.find_last_of('\\', dir.size() - 2);
    return k == std::string::npos ? std::string() : dir.substr(0, k + 1);
}

std::string dir_name(const std::string& dir) {
    const size_t k = dir.find_last_of('\\', dir.size() - 2);
    return dir.substr(k == std::string::npos ? 0 : k + 1, dir.size() - 1 - (k == std::string::npos ? 0 : k + 1));
}

// LR2 のノーツ数（LR2 の数え方とは細かく違う譜面がある。手元の LR2 の song.db とは 7 割ほどが同じ）
int lr2_notes(const ChartRecord& c) { return c.notes; }

// BMjoy のコースの鍵（頭の 32 文字が、11 文字目の種類のほかは 0。続けてステージの md5）
bool is_course_key(const std::string& k) {
    if (k.size() <= 32 || k.size() % 32 != 0) return false;
    for (size_t i = 0; i < 32; ++i)
        if (i != 10 && k[i] != '0') return false;
    return true;
}

std::string fold_case(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Db {
    sqlite3* db = nullptr;
    std::string error;
    explicit Db(const std::string& path) {
        std::error_code ec;
        fs::create_directories(fs::u8path(path).parent_path(), ec);
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) error = "開けない: " + path;
        else sqlite3_busy_timeout(db, 5000);
    }
    ~Db() { sqlite3_close(db); }
    bool exec(const char* sql) {
        char* msg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
        error = msg ? msg : "SQL が失敗した";
        sqlite3_free(msg);
        return false;
    }
};

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
    void i64(int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
    bool run() {
        const bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        return ok;
    }
};

}  // namespace

std::string lr2_folder_crc(const std::string& dir_utf8) {
    std::string b = utf8_to_cp932(lr2_dir(dir_utf8));  // 番号は Shift_JIS のパスから（表に入れる文字は UTF-8）
    b.push_back('\0');
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", lr2_crc32(b));
    return buf;
}

bool export_lr2_song_db(const std::string& path, const std::vector<ChartRecord>& charts, const std::vector<std::string>& roots,
                        std::string* error) {
    Db d(path);
    auto fail = [&] {
        if (error) *error = d.error;
        return false;
    };
    if (!d.db) return fail();
    // LR2 と同じ形の表（無ければ作る。ほかのツールが足した表には触らない）
    if (!d.exec("CREATE TABLE IF NOT EXISTS song(hash TEXT ,title TEXT ,subtitle TEXT ,genre TEXT,artist TEXT,subartist TEXT,tag TEXT ,"
                "path TEXT primary key ,type INTEGER,folder TEXT,stagefile TEXT,banner TEXT,backbmp TEXT,parent TEXT,level INTEGER,"
                "difficulty INTEGER,maxbpm INTEGER,minbpm INTEGER,mode INTEGER,judge INTEGER,longnote INTEGER,bga INTEGER,random INTEGER,"
                "date INTEGER,favorite INTEGER,txt INTEGER,karinotes INTEGER,adddate INTEGER,exlevel INTEGER);"
                "CREATE INDEX IF NOT EXISTS hashidx ON song (hash);"
                "CREATE INDEX IF NOT EXISTS parentidx ON song (parent);"
                "CREATE TABLE IF NOT EXISTS folder(title TEXT ,subtitle TEXT ,category TEXT,info_a TEXT,info_b TEXT,command TEXT,"
                "path TEXT primary key,type INTEGER,banner TEXT,parent TEXT,date INTEGER,max INTEGER,adddate INTEGER);"
                "BEGIN;DELETE FROM song;DELETE FROM folder;"))
        return fail();
    std::set<std::string> root_keys;
    for (const std::string& r : roots) root_keys.insert(fold_case(lr2_dir(r)));
    // 譜面のフォルダと、登録した曲フォルダまでの途中のフォルダ
    std::map<std::string, int64_t> folders;  // フォルダ → 見つけた日時
    std::unordered_map<std::string, bool> is_root;
    auto root_of = [&](const std::string& dir) {
        auto [it, fresh] = is_root.try_emplace(dir, false);
        if (fresh) it->second = root_keys.count(fold_case(dir)) > 0;
        return it->second;
    };
    Stmt song(d.db, "INSERT OR REPLACE INTO song(hash,title,subtitle,genre,artist,subartist,path,type,folder,stagefile,banner,backbmp,"
                    "parent,level,difficulty,maxbpm,minbpm,mode,judge,longnote,bga,random,date,favorite,txt,karinotes,adddate,exlevel) "
                    "VALUES(?,?,?,?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?,?,0,0,?,0,0,?,?,0)");
    for (const ChartRecord& c : charts) {
        const std::string dir = lr2_dir(c.folder);
        const std::string parent = parent_dir(dir);
        song.text(1, c.md5), song.text(2, c.title), song.text(3, c.subtitle), song.text(4, c.genre), song.text(5, c.artist);
        song.text(6, c.subartist), song.text(7, lr2_file(c.path)), song.text(8, lr2_folder_crc(dir));
        song.text(9, c.stagefile), song.text(10, c.banner), song.text(11, c.backbmp);
        song.text(12, root_of(dir) || parent.empty() ? kLr2RootParent : lr2_folder_crc(parent));
        song.i64(13, c.playlevel), song.i64(14, c.difficulty);
        song.i64(15, static_cast<int64_t>(c.bpm_max)), song.i64(16, static_cast<int64_t>(c.bpm_min)), song.i64(17, c.keys);
        song.i64(18, c.rank), song.i64(19, c.ln + c.ln_free > 0 ? 1 : 0), song.i64(20, c.added), song.i64(21, lr2_notes(c));
        song.i64(22, c.added);
        if (!song.run()) return d.error = sqlite3_errmsg(d.db), fail();
        // 曲フォルダの一番上まで、フォルダの行を足す
        for (std::string f = dir; !f.empty(); f = parent_dir(f)) {
            auto [it, fresh] = folders.try_emplace(f, c.added);
            if (!fresh) {
                it->second = std::min(it->second, c.added);
                break;
            }
            if (root_of(f)) break;
        }
    }
    Stmt folder(d.db, "INSERT OR REPLACE INTO folder(title,path,type,parent,date,adddate) VALUES(?,?,1,?,?,?)");
    for (const auto& [f, added] : folders) {
        const std::string parent = parent_dir(f);
        folder.text(1, dir_name(f)), folder.text(2, f);
        folder.text(3, root_of(f) || parent.empty() || !folders.count(parent) ? kLr2RootParent : lr2_folder_crc(parent));
        folder.i64(4, added), folder.i64(5, added);
        if (!folder.run()) return d.error = sqlite3_errmsg(d.db), fail();
    }
    if (!d.exec("COMMIT;")) return fail();
    return true;
}

bool export_lr2_scores(const std::string& path, const std::string& player, const std::vector<ChartRecord>& charts,
                       const std::vector<ScoreRecord>& scores, const PlayerStats& stats, const Lr2Carry& carry,
                       std::string* error) {
    Db d(path);
    auto fail = [&] {
        if (error) *error = d.error;
        return false;
    };
    if (!d.db) return fail();
    if (!d.exec("CREATE TABLE IF NOT EXISTS player(id TEXT primary key,hash TEXT,name TEXT,irid INTEGER,irname TEXT,playcount INTEGER,"
                "clear INTEGER,fail INTEGER,perfect INTEGER,great INTEGER,good INTEGER,bad INTEGER,poor INTEGER,playtime INTEGER,"
                "combo INTEGER,maxcombo INTEGER,grade_7 INTEGER,grade_5 INTEGER,grade_14 INTEGER,grade_10 INTEGER,grade_9 INTEGER,"
                "trial INTEGER,option INTEGER,systemversion INTEGER,gradeversion INTEGER, trialversion INTEGER,scorehash TEXT);"
                "CREATE TABLE IF NOT EXISTS score(hash TEXT primary key,clear INTEGER,perfect INTEGER,great INTEGER,good INTEGER,"
                "bad INTEGER,poor INTEGER,totalnotes INTEGER,maxcombo INTEGER,minbp INTEGER,playcount INTEGER,clearcount INTEGER,"
                "failcount INTEGER,rank INTEGER,rate INTEGER,clear_db INTEGER,op_history INTEGER,scorehash TEXT,ghost TEXT,"
                "clear_sd INTEGER,clear_ex INTEGER,op_best INTEGER, rseed INTEGER, complete INTEGER);"
                "CREATE INDEX IF NOT EXISTS hashidx ON score (hash);"
                "BEGIN;DELETE FROM score;DELETE FROM player;"))
        return fail();
    std::unordered_map<std::string, const ChartRecord*> by_sha;
    for (const ChartRecord& c : charts) by_sha.emplace(c.sha256, &c);
    Stmt st(d.db, "INSERT OR REPLACE INTO score(hash,clear,perfect,great,good,bad,poor,totalnotes,maxcombo,minbp,playcount,clearcount,"
                  "failcount,rank,rate,clear_db,op_history,scorehash,ghost,clear_sd,clear_ex,op_best,rseed,complete) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,'',?,0,0,?,?,?)");
    for (const ScoreRecord& s : scores) {
        // 譜面は md5 で、コース（LR2 のコースの鍵。md5 を並べたもの）はそのままの鍵で
        std::string hash;
        int notes = 0;
        int total = 0;
        if (auto it = by_sha.find(s.sha256); it != by_sha.end()) hash = it->second->md5, notes = it->second->notes, total = lr2_notes(*it->second);
        else if (is_course_key(s.sha256)) {
            auto k = carry.course_keys.find(s.sha256);  // LR2 から引き継いだコースは、LR2 の元の鍵で
            hash = k != carry.course_keys.end() ? k->second : s.sha256;
        }
        if (hash.empty()) continue;
        const int ex = s.judge[0] * 2 + s.judge[1];
        const double ratio = notes > 0 ? static_cast<double>(ex) / (notes * 2) : 0;
        st.text(1, hash), st.i64(2, std::min(s.clear, 5));  // LR2 の表に PERFECT は無い（FULL COMBO のうちの 1 つ）
        for (int j = 0; j < 5; ++j) st.i64(3 + j, s.judge[j]);
        st.i64(8, total), st.i64(9, s.max_combo), st.i64(10, std::max(0, s.bp));
        st.i64(11, s.playcount), st.i64(12, s.clearcount), st.i64(13, std::max(0, s.playcount - s.clearcount));
        st.i64(14, std::clamp(static_cast<int>(ratio * 9), 0, 9)), st.i64(15, std::clamp(static_cast<int>(ratio * 100), 0, 100));
        st.i64(16, s.op_history), st.text(17, s.ghost), st.i64(18, s.op_best), st.i64(19, std::max(0, s.rseed));
        st.i64(20, s.playcount > 0 && s.clear >= 2 ? 1 : 0);
        if (!st.run()) return d.error = sqlite3_errmsg(d.db), fail();
    }
    // プレイヤー：引き継いだ欄はそのまま（ID・名前・IR・段位・プレイ時間など）、累計は今の値
    std::map<std::string, std::string> row = {{"id", player}, {"name", player}, {"systemversion", "1"}};
    for (const auto& [k, v] : carry.player) row[k] = v;
    row["playcount"] = std::to_string(stats.playcount), row["clear"] = std::to_string(stats.clear), row["fail"] = std::to_string(stats.fail);
    static const char* judges[] = {"perfect", "great", "good", "bad", "poor"};
    for (int j = 0; j < 5; ++j) row[judges[j]] = std::to_string(stats.judge[j]);
    std::set<std::string> cols;  // 表にある欄だけを書く
    {
        Stmt info(d.db, "PRAGMA table_info(player)");
        while (sqlite3_step(info.s) == SQLITE_ROW) cols.insert(reinterpret_cast<const char*>(sqlite3_column_text(info.s, 1)));
    }
    std::string names, marks;
    std::vector<std::string> values;
    for (const auto& [k, v] : row) {
        if (!cols.count(k)) continue;
        names += (names.empty() ? "\"" : ",\"") + k + "\"", marks += marks.empty() ? "?" : ",?";
        values.push_back(v);
    }
    Stmt pl(d.db, ("INSERT OR REPLACE INTO player(" + names + ") VALUES(" + marks + ")").c_str());
    for (size_t j = 0; j < values.size(); ++j) pl.text(static_cast<int>(j) + 1, values[j]);  // 数の欄は SQLite が数にする
    if (!pl.run()) return d.error = sqlite3_errmsg(d.db), fail();
    if (!d.exec("COMMIT;")) return fail();
    return true;
}

bool export_lr2_files(const SongDb& db, const std::string& database_dir, const std::vector<std::string>& roots,
                      std::string player, bool songs, std::string* error) {
    Lr2Carry carry{db.lr2_player_info(), db.lr2_course_keys()};
    if (player.empty())
        if (auto it = carry.player.find("id"); it != carry.player.end()) player = it->second;
    if (player.empty()) player = player_id().empty() ? "BMjoy" : player_id();
    std::vector<ChartRecord> charts = db.all_charts();
    fill_difficulties(charts);  // LR2 は #DIFFICULTY の無い譜面の難易度を補って入れている
    if (songs && !export_lr2_song_db(database_dir + "/song.db", charts, roots, error)) return false;
    return export_lr2_scores(database_dir + "/Score/" + player + ".db", player, charts, db.all_scores(0, 0), db.player_stats(), carry,
                             error);
}

}  // namespace bmj
