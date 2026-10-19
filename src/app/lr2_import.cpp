#include "app/lr2_import.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>

#include "util/text.h"

namespace fs = std::filesystem;

namespace bmj {

namespace {
std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}
}  // namespace

Lr2ImportResult import_from_lr2(const std::string& lr2_dir, AppConfig& cfg, SongDb& db,
                                const std::function<void(const std::string&)>& status) {
    Lr2ImportResult res;
    fs::path root = fs::path(lr2_dir) / "LR2files";
    std::string xml = read_all(root / "Config" / "config.xmh");
    if (xml.empty()) xml = read_all(root / "Config" / "config.xml");
    if (xml.empty()) return res.error = "LR2 の設定が見つからない（" + (root / "Config").string() + "）", res;
    xml = to_utf8_auto(xml);
    // 曲フォルダ（LR2 のカスタムフォルダの置き場は除く）
    std::vector<std::string> dirs;
    static const std::regex path_re("<path>([^<]*)</path>");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), path_re); it != std::sregex_iterator(); ++it) {
        std::string p = (*it)[1].str();
        std::string low = p;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("lr2files") != std::string::npos) continue;
        while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        dirs.push_back(p);
    }
    std::smatch m;
    static const std::regex id_re("<player>\\s*<id>([^<]*)</id>");
    if (std::regex_search(xml, m, id_re)) res.player = m[1].str();
    if (dirs.empty()) return res.error = "LR2 の設定に曲フォルダが無い", res;
    cfg.song_dirs = dirs;
    res.song_dirs = static_cast<int>(dirs.size());

    // 難易度表（すでに登録してあるものは足さない）
    for (const std::string& u : SongDb::lr2_table_urls((root / "Database" / "song.db").string()))
        if (std::find(cfg.tables.begin(), cfg.tables.end(), u) == cfg.tables.end()) cfg.tables.push_back(u), res.tables++;
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
