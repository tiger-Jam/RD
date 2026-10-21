#include "app/lr2_import.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

#include "app/skin_catalog.h"
#include "db/difficulty_table.h"
#include "sqlite3.h"
#include "util/hash.h"
#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {
std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

int import_lr2_skin_custom(const std::string& lr2_dir, const std::string& skin_root, AppConfig& cfg) {
    fs::path dir = fs::path(lr2_dir) / "LR2files" / "SkinCustomize";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    SkinCatalog cat;
    cat.scan(skin_root.empty() ? lr2_dir : skin_root);
    fs::path root = fs::path(skin_root.empty() ? lr2_dir : skin_root);
    static const std::regex config_re("<config_(\\d+)>\\s*(-?\\d+)\\s*</config_"), file_re("<filename_(\\d+)>([^<]*)</filename_");
    int count = 0;
    for (const auto& [type, list] : cat.entries())
        for (const SkinEntry& e : list) {
            const bool play_skin = type >= 0 && type <= 4;  // プレイスキン（位置の調整も引き継ぐ）
            if (e.header.customs.empty() && !play_skin) continue;
            // LR2 はスキンを「LR2files\\Theme\\…」の形（CP932）で覚え、その MD5 をファイル名にする
            std::u8string rel = fs::path(e.path).lexically_relative(root).u8string();
            std::string key(rel.begin(), rel.end());
            std::replace(key.begin(), key.end(), '/', '\\');
            std::string xml = read_all(dir / (md5_hex(utf8_to_cp932(key)) + ".xml"));
            if (xml.empty()) continue;
            xml = to_utf8_auto(xml);
            if (play_skin) {
                // 位置の調整（LR2: SkinCustomize の shift・rate・judge・size・dark・note_1p）
                auto num = [&](const char* group, const char* item, int def) {
                    std::smatch m;
                    std::regex re(std::string("<") + group + ">[\\s\\S]*?<" + item + ">\\s*(-?\\d+)\\s*</" + item + ">[\\s\\S]*?</" + group + ">");
                    return std::regex_search(xml, m, re) ? std::stoi(m[1].str()) : def;
                };
                SkinAdjust a;
                a.shift_x = num("shift", "x", 0), a.shift_y = num("shift", "y", 0);
                a.rate_x = num("rate", "x", 100), a.rate_y = num("rate", "y", 100);
                a.judge_x = num("judge", "x", 0), a.judge_y = num("judge", "y", 0);
                a.size_x = num("size", "x", 0), a.size_y = num("size", "y", 0);
                a.dark = std::clamp(num("dark", "type", 0), 0, 2);
                a.note_x = num("note_1p", "x", 0), a.note_y = num("note_1p", "y", 0);
                if (!a.is_default()) cfg.skin_adjust[e.path] = a;
                else cfg.skin_adjust.erase(e.path);
            }
            if (e.header.customs.empty()) {
                count++;
                continue;
            }
            std::map<int, long long> values;
            std::map<int, std::string> files;
            for (auto it = std::sregex_iterator(xml.begin(), xml.end(), config_re); it != std::sregex_iterator(); ++it)
                values[std::stoi((*it)[1].str())] = std::stoll((*it)[2].str());
            for (auto it = std::sregex_iterator(xml.begin(), xml.end(), file_re); it != std::sregex_iterator(); ++it)
                files[std::stoi((*it)[1].str())] = (*it)[2].str();
            // 項目の番号は #CUSTOMOPTION と #CUSTOMFILE で共通。#CUSTOMOPTION は選択肢の番号、#CUSTOMFILE は名前で残す。
            // 選択肢に無い値は既定のまま
            SkinHeader h = e.header;
            for (size_t i = 0; i < h.customs.size(); ++i) {
                SkinCustomItem& c = h.customs[i];
                if (!c.is_file) {
                    auto it = values.find(static_cast<int>(i));
                    if (it != values.end() && it->second >= c.op_start && it->second < c.op_start + static_cast<long long>(c.labels.size()))
                        c.selected = static_cast<int>(it->second - c.op_start);
                } else if (auto it = files.find(static_cast<int>(i)); it != files.end()) {
                    for (size_t j = 0; j < c.labels.size(); ++j)
                        if (lower(c.labels[j]) == lower(it->second)) c.selected = static_cast<int>(j);
                }
            }
            cfg.skin_custom[e.path] = skin_custom_choices(h);
            count++;
        }
    return count;
}

Lr2ImportResult import_from_lr2(const std::string& lr2_dir, AppConfig& cfg, SongDb& db,
                                const std::function<void(const std::string&)>& status, const std::string& skin_root) {
    Lr2ImportResult res;
    fs::path root = fs::path(lr2_dir) / "LR2files";
    std::string xml = read_all(root / "Config" / "config.xmh");
    if (xml.empty()) xml = read_all(root / "Config" / "config.xml");
    if (xml.empty()) return res.error = "LR2 の設定が見つからない（" + (root / "Config").string() + "）", res;
    xml = to_utf8_auto(xml);
    // 曲フォルダ（LR2 のカスタムフォルダの置き場は除く）
    std::vector<std::string> dirs, custom_dirs;
    static const std::regex path_re("<path>([^<]*)</path>");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), path_re); it != std::sregex_iterator(); ++it) {
        std::string p = (*it)[1].str();
        std::string low = p;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        // LR2files の下はカスタムフォルダの置き場（曲フォルダではない）
        if (low.find("lr2files") != std::string::npos) {
            custom_dirs.push_back(p);
            continue;
        }
        dirs.push_back(p);
    }
    std::smatch m;
    static const std::regex id_re("<player>\\s*<id>([^<]*)</id>");
    if (std::regex_search(xml, m, id_re)) res.player = m[1].str();
    if (dirs.empty()) return res.error = "LR2 の設定に曲フォルダが無い", res;
    cfg.song_dirs = dirs;
    cfg.custom_folder_dirs = custom_dirs;
    res.song_dirs = static_cast<int>(dirs.size());

    // 難易度表（すでに登録してあるものは足さない）
    for (const std::string& u : SongDb::lr2_table_urls((root / "Database" / "song.db").string()))
        if (std::find(cfg.tables.begin(), cfg.tables.end(), u) == cfg.tables.end()) cfg.tables.push_back(u), res.tables++;
    if (status) status("LR2 のスキンのカスタマイズを引き継いでいます");
    res.skins = import_lr2_skin_custom(lr2_dir, skin_root, cfg);
    if (status) status("LR2 の曲の情報を取り込んでいます");
    std::string err;
    int n = db.import_lr2_songs((root / "Database" / "song.db").string(), &err, {}, dirs);
    if (n < 0) return res.error = err, res;
    res.charts = n;
    if (status) status("LR2 の段位とコースを引き継いでいます");
    res.courses = std::max(0, import_lr2_courses((root / "Database" / "song.db").string()));
    if (!res.player.empty()) {
        if (status) status("LR2 のスコアを取り込んでいます");
        fs::path score = root / "Database" / "Score" / (res.player + ".db");
        if (fs::exists(score)) {
            int s = db.import_lr2_scores(score.string(), &err);
            if (s >= 0) res.scores = s;
        }
    }
    res.ok = true;
    return res;
}

namespace {

// パスを比べるための形（区切りを / にそろえ、英字は小文字。Windows は大文字小文字を区別しないため）
std::string path_key(const fs::path& p) {
    std::string k = p.lexically_normal().generic_string();
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

// p が root の下なら、root からの相対パスを返す
bool under(const fs::path& p, const fs::path& root, fs::path& rel) {
    std::string kp = path_key(p), kr = path_key(root);
    if (kp.size() < kr.size() || kp.compare(0, kr.size(), kr) != 0 || (kp.size() > kr.size() && kp[kr.size()] != '/')) return false;
    rel = fs::path(p).lexically_normal().lexically_relative(fs::path(root).lexically_normal());
    if (rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0) {
        // 大文字小文字の違いで lexically_relative が使えないときは、文字数で切る
        std::string g = fs::path(p).lexically_normal().generic_string();
        rel = fs::path(g.substr(std::min(g.size(), path_key(root).size() + 1)));
    }
    return true;
}

}  // namespace

AssetCopyResult copy_lr2_assets(AppConfig& cfg, const std::string& dst_root, const std::function<void(const std::string&)>& status) {
    AssetCopyResult res;
    std::error_code ec;
    const fs::path src = cfg.lr2_root;
    const fs::path dst = dst_root;
    if (cfg.lr2_root.empty() || !fs::is_directory(src / "LR2files", ec)) return res.error = "LR2 のフォルダが見つからない：" + cfg.lr2_root, res;
    if (path_key(src) == path_key(dst)) return res.error = "すでに BMjoy のフォルダの素材を使っている", res;

    // 写すもの：見た目と音にかかわるものだけ（曲・記録・IR のデータは BMjoy が別に持つ）
    static const char* kDirs[] = {"Theme", "Sound", "Bgm", "WallPaper", "Config", "Mouse", "Movie", "Help"};
    auto copy_tree = [&](const fs::path& from, const fs::path& to) {
        if (!fs::is_directory(from, ec)) return;
        for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            fs::path rel = it->path().lexically_relative(from);
            fs::path out = to / rel;
            fs::create_directories(out.parent_path(), e2);
            // すでに同じ大きさ・同じ日時以降のものがあれば写さない（何度実行してもよい）
            if (fs::exists(out, e2) && fs::file_size(out, e2) == it->file_size(e2) &&
                fs::last_write_time(out, e2) >= it->last_write_time(e2))
                continue;
            if (fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, e2)) {
                res.files++;
                res.bytes += static_cast<int64_t>(it->file_size(e2));
                if (status && res.files % 200 == 0) status("素材を写しています（" + std::to_string(res.files) + " ファイル）");
            } else {
                res.failed++;
            }
        }
        ec.clear();
    };
    for (const char* d : kDirs) copy_tree(src / "LR2files" / d, dst / "LR2files" / d);

    // カスタムフォルダの置き場：LR2 の LR2files の下にあるものは、同じ相対の場所に写して付け替える
    for (std::string& d : cfg.custom_folder_dirs) {
        fs::path p = d, rel;
        for (fs::path cur = p; !cur.empty() && cur != cur.parent_path(); cur = cur.parent_path()) {
            if (path_key(cur.filename()) != "lr2files") continue;
            if (!under(p, cur, rel)) break;
            copy_tree(p, dst / "LR2files" / rel);
            d = (dst / "LR2files" / rel).string();
            break;
        }
    }

    // 設定の中の素材の場所を、写した先に直す
    auto relocate = [&](std::string& path) {
        fs::path rel;
        if (!path.empty() && path != "none" && under(path, src, rel)) path = (dst / rel).string();
    };
    for (std::string* p : {&cfg.play_skin, &cfg.result_skin, &cfg.courseresult_skin, &cfg.select_skin, &cfg.decide_skin,
                           &cfg.keyconfig_skin, &cfg.sound_set})
        relocate(*p);
    auto relocate_keys = [&](auto& m) {
        std::remove_reference_t<decltype(m)> out;
        for (auto& [k, v] : m) {
            std::string nk = k;
            relocate(nk);
            out[nk] = v;
        }
        m.swap(out);
    };
    relocate_keys(cfg.skin_custom);
    relocate_keys(cfg.skin_adjust);
    cfg.lr2_root = dst.string();
    res.ok = true;
    return res;
}

int import_lr2_courses(const std::string& lr2_song_db) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(lr2_song_db.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    std::vector<TableCourse> courses;
    for (const char* table : {"grade", "expert"}) {
        std::string sql = std::string("SELECT title, hash FROM ") + table + " ORDER BY id";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            const unsigned char* h = sqlite3_column_text(st, 1);
            if (!t || !h) continue;
            TableCourse c;
            c.name = to_utf8_auto(reinterpret_cast<const char*>(t));
            if (std::string(table) == "grade") c.constraints = {"grade_mirror", "gauge_lr2"};
            // hash は「コースの印 32 文字」のあとに、ステージの譜面の md5 が 32 文字ずつ続く
            std::string hash = reinterpret_cast<const char*>(h);
            for (size_t i = 32; i + 32 <= hash.size(); i += 32) c.charts.push_back(TableChart{}), c.charts.back().md5 = hash.substr(i, 32);
            if (!c.charts.empty()) courses.push_back(std::move(c));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!write_lr2_courses(lr2_courses_path(), courses)) return -1;
    return static_cast<int>(courses.size());
}

}  // namespace bmj
