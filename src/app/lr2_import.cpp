#include "app/lr2_import.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

#include "app/skin_catalog.h"
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

}  // namespace bmj
