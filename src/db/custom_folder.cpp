#include "db/custom_folder.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

#include "sqlite3.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}

// ファイルの更新日時（BMjoy の曲データベースの値）を UNIX 秒にする（LR2 の song.date）
int64_t to_unix(int64_t file_ticks) {
#if defined(_MSC_VER) || (defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L && !defined(__APPLE__))
    auto ft = std::chrono::file_clock::time_point(std::chrono::file_clock::duration(file_ticks));
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(ft).time_since_epoch()).count();
#else
    auto ft = std::chrono::file_clock::time_point(std::chrono::file_clock::duration(file_ticks));
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(ft).time_since_epoch()).count();
#endif
}

// 改行（CRLF・LF・CR）で行に分ける
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r' || c == '\n') {
            out.push_back(cur), cur.clear();
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool skip_command(const std::string& cmd) {
    std::string l = cmd;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("playlist_entry") != std::string::npos || l.find("__") != std::string::npos;
}

}  // namespace

bool parse_lr2folder(const std::string& path, CustomFolderDef& out) {
    std::string text = to_utf8_auto(read_file(path));
    if (text.empty()) return false;
    out = CustomFolderDef{};
    bool has_command = false;
    for (const std::string& raw : split_lines(text)) {
        std::string line(trim(raw));
        auto value = [&](size_t n) { return std::string(trim(std::string_view(line).substr(n))); };
        std::string up = to_upper_ascii(line);
        if (up.rfind("#COMMAND", 0) == 0) out.command = value(8), has_command = true;
        else if (up.rfind("#MAXTRACKS", 0) == 0) out.max_tracks = std::atoi(value(10).c_str());
        else if (up.rfind("#CATEGORY", 0) == 0) out.category = value(9);
        else if (up.rfind("#TITLE", 0) == 0) out.title = value(6);
    }
    if (out.title.empty()) out.title = fs::path(path).stem().string();
    return has_command;
}

std::vector<CustomFolderNode> load_custom_folders(const std::string& dir) {
    std::vector<CustomFolderNode> out;
    std::error_code ec;
    std::vector<fs::path> items;
    for (const auto& e : fs::directory_iterator(dir, ec)) items.push_back(e.path());
    std::sort(items.begin(), items.end());
    for (const fs::path& p : items) {
        if (fs::is_directory(p, ec)) {
            // 中の .lr2folder を名前の順に。名前は folderinfo.txt の #TITLE（無ければディレクトリ名）
            CustomFolderNode n;
            n.group = true;
            n.title = p.filename().string();
            std::string info = to_utf8_auto(read_file(p / "folderinfo.txt"));
            for (const std::string& raw : split_lines(info)) {
                std::string line(trim(raw));
                if (to_upper_ascii(line).rfind("#TITLE", 0) == 0) n.title = std::string(trim(std::string_view(line).substr(6)));
            }
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(p, ec))
                if (to_upper_ascii(e.path().extension().string()) == ".LR2FOLDER") files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) {
                CustomFolderDef d;
                if (parse_lr2folder(f.string(), d) && !skip_command(d.command)) n.defs.push_back(std::move(d));
            }
            if (!n.defs.empty()) out.push_back(std::move(n));
        } else if (to_upper_ascii(p.extension().string()) == ".LR2FOLDER") {
            CustomFolderNode n;
            CustomFolderDef d;
            if (!parse_lr2folder(p.string(), d) || skip_command(d.command)) continue;
            n.title = d.title;
            n.defs.push_back(std::move(d));
            out.push_back(std::move(n));
        }
    }
    return out;
}

CustomFolderQuery::~CustomFolderQuery() {
    if (db_) sqlite3_close(db_);
}

namespace {
const char* kInsertScore =
    "INSERT OR REPLACE INTO score(hash, clear, perfect, great, good, bad, poor, totalnotes, maxcombo, minbp, playcount, "
    "clearcount, failcount, rank, rate, clear_db, op_history, clear_sd, clear_ex, op_best, rseed, complete) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,0,0,?,?,1)";
}  // namespace

bool CustomFolderQuery::build(const std::vector<ChartRecord>& charts, const std::unordered_map<std::string, ScoreRecord>& scores,
                              const std::unordered_map<std::string, int>& favorites) {
    if (db_) sqlite3_close(db_), db_ = nullptr;
    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) return false;
    // LR2 の song 表と score 表と同じ欄（LR2: songmanage:2693, 2696）
    const char* schema =
        "CREATE TABLE song(hash TEXT, title TEXT, subtitle TEXT, genre TEXT, artist TEXT, subartist TEXT, tag TEXT, path TEXT, "
        "type INTEGER, folder TEXT, stagefile TEXT, banner TEXT, backbmp TEXT, parent TEXT, level INTEGER, difficulty INTEGER, "
        "maxbpm INTEGER, minbpm INTEGER, mode INTEGER, judge INTEGER, longnote INTEGER, bga INTEGER, random INTEGER, date INTEGER, "
        "favorite INTEGER, txt INTEGER, karinotes INTEGER, adddate INTEGER, exlevel INTEGER);"
        "CREATE TABLE score(hash TEXT PRIMARY KEY, clear INTEGER, perfect INTEGER, great INTEGER, good INTEGER, bad INTEGER, "
        "poor INTEGER, totalnotes INTEGER, maxcombo INTEGER, minbp INTEGER, playcount INTEGER, clearcount INTEGER, "
        "failcount INTEGER, rank INTEGER, rate INTEGER, clear_db INTEGER, op_history INTEGER, scorehash TEXT, ghost TEXT, "
        "clear_sd INTEGER, clear_ex INTEGER, op_best INTEGER, rseed INTEGER, complete INTEGER);";
    if (sqlite3_exec(db_, schema, nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* song = nullptr;
    sqlite3_stmt* score = nullptr;
    sqlite3_prepare_v2(db_,
                       "INSERT INTO song(hash, title, subtitle, genre, artist, subartist, tag, path, type, folder, stagefile, banner, "
                       "backbmp, parent, level, difficulty, maxbpm, minbpm, mode, judge, longnote, bga, random, date, favorite, txt, "
                       "karinotes, adddate, exlevel) VALUES(?,?,?,?,?,?,'',?,0,?,?,?,?,?,?,?,?,?,?,?,?,0,0,?,?,0,?,?,0)",
                       -1, &song, nullptr);
    sqlite3_prepare_v2(db_, kInsertScore, -1, &score, nullptr);
    auto text = [](sqlite3_stmt* s, int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); };
    std::set<std::string> seen;
    for (const ChartRecord& c : charts) {
        if (!seen.insert(c.md5).second) continue;
        auto fav = favorites.find(c.md5);
        text(song, 1, c.md5), text(song, 2, c.title), text(song, 3, c.subtitle), text(song, 4, c.genre), text(song, 5, c.artist);
        text(song, 6, c.subartist), text(song, 7, c.path), text(song, 8, c.folder), text(song, 9, c.stagefile), text(song, 10, c.banner);
        text(song, 11, c.backbmp), text(song, 12, c.folder);
        sqlite3_bind_int(song, 13, c.playlevel);
        sqlite3_bind_int(song, 14, c.difficulty);
        sqlite3_bind_int(song, 15, static_cast<int>(c.bpm_max));
        sqlite3_bind_int(song, 16, static_cast<int>(c.bpm_min));
        sqlite3_bind_int(song, 17, c.keys);
        sqlite3_bind_int(song, 18, c.rank);
        sqlite3_bind_int(song, 19, c.ln > 0 ? 1 : 0);
        sqlite3_bind_int64(song, 20, c.mtime ? to_unix(c.mtime) : 0);
        sqlite3_bind_int(song, 21, fav != favorites.end() ? fav->second : 0);
        sqlite3_bind_int(song, 22, c.notes);
        sqlite3_bind_int64(song, 23, c.added);
        sqlite3_step(song);
        sqlite3_reset(song);
        auto sc = scores.find(c.sha256);
        if (sc == scores.end()) continue;
        bind_score(score, c, sc->second);
        sqlite3_step(score);
        sqlite3_reset(score);
    }
    sqlite3_finalize(song);
    sqlite3_finalize(score);
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return true;
}

void CustomFolderQuery::bind_score(sqlite3_stmt* score, const ChartRecord& c, const ScoreRecord& r) {
        const int notes = std::max(1, c.notes);
        sqlite3_bind_text(score, 1, c.md5.c_str(), static_cast<int>(c.md5.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(score, 2, std::min(r.clear, 5));  // LR2 のランプは FULL COMBO（5）まで
        for (int i = 0; i < 5; ++i) sqlite3_bind_int(score, 3 + i, r.judge[i]);
        sqlite3_bind_int(score, 8, c.notes);
        sqlite3_bind_int(score, 9, r.max_combo);
        sqlite3_bind_int(score, 10, r.bp);
        sqlite3_bind_int(score, 11, r.playcount);
        sqlite3_bind_int(score, 12, r.clearcount);
        sqlite3_bind_int(score, 13, r.playcount - r.clearcount);
        sqlite3_bind_int(score, 14, std::clamp(r.exscore * 9 / (notes * 2), 0, 8));
        sqlite3_bind_int(score, 15, r.exscore * 100 / (notes * 2));
        sqlite3_bind_int64(score, 16, r.op_history);
        sqlite3_bind_int(score, 17, r.op_best);
        sqlite3_bind_int(score, 18, r.rseed);
}

void CustomFolderQuery::update_score(const ChartRecord& c, const ScoreRecord& r) {
    std::lock_guard<std::mutex> db_lock(db_mu_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, kInsertScore, -1, &st, nullptr) == SQLITE_OK) {
        bind_score(st, c, r);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

bool CustomFolderQuery::cached(const CustomFolderDef& def, std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(def.command + "\x1f" + std::to_string(def.max_tracks));
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
}

std::vector<std::string> CustomFolderQuery::run(const CustomFolderDef& def, std::string* error) const {
    std::vector<std::string> out;
    if (def.max_tracks != 1 && cached(def, out)) return out;
    std::lock_guard<std::mutex> db_lock(db_mu_);
    if (!db_) return out;
    std::string sql = "SELECT song.hash, song.favorite FROM song LEFT JOIN score ON song.hash = score.hash WHERE " + def.command;
    // #MAXTRACKS 1 は 1 曲を無作為に選ぶ（LR2: songmanage:2091）。IGNORE の譜面は、IGNORE を集めるフォルダ以外では出さない
    if (def.max_tracks == 1 && def.command.find("random()") == std::string::npos) sql += " ORDER BY random()";
    const bool ignore_folder = def.command.find("favorite = 2") != std::string::npos;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (def.max_tracks > 0 && static_cast<int>(out.size()) >= def.max_tracks) break;
        if (sqlite3_column_int(st, 1) == 2 && !ignore_folder) continue;  // IGNORE（LR2: songmanage:821）
        const unsigned char* h = sqlite3_column_text(st, 0);
        if (h) out.emplace_back(reinterpret_cast<const char*>(h));
    }
    sqlite3_finalize(st);
    if (def.max_tracks != 1) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_[def.command + "\x1f" + std::to_string(def.max_tracks)] = out;
    }
    return out;
}

}  // namespace bmj
