// 曲データベース（仕様書8章）。LR2とは別ファイルで、最初からWALで運用する。
// 書き込みはまとめて1トランザクションで行う
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct sqlite3;

namespace bmj {

struct ChartRecord {
    std::string path, folder;  // 譜面ファイルと、それがあるフォルダ
    std::string sha256, md5;
    std::string title, subtitle, artist, subartist, genre;
    int playlevel = 0, difficulty = 0;
    int notes = 0, ln = 0, rank = 2, total = 0;
    double bpm_min = 0, bpm_max = 0, bpm_main = 0;
    int length_ms = 0;
    bool scratch = true;
    int64_t mtime = 0, size = 0;
};

struct ScoreRecord {
    std::string sha256;
    int clear = 0;  // LR2のランプ（0=NO PLAY, 1=FAILED, 2=EASY, 3=NORMAL, 4=HARD, 5=FULL COMBO）
    int exscore = 0, bp = -1, max_combo = 0;
    int playcount = 0, clearcount = 0;
    int64_t last_played = 0;
};

struct ScanStats {
    int found = 0, added = 0, updated = 0, removed = 0, failed = 0;
    double seconds = 0;
};

class SongDb {
public:
    ~SongDb();
    bool open(const std::string& path);
    void close();

    // 曲フォルダを読み直す。変わったファイルだけを並列にパースし、1トランザクションで書く（仕様書8章「リロード処理」）
    ScanStats scan(const std::vector<std::string>& dirs, int threads,
                   const std::function<void(int done, int total)>& progress = nullptr);

    std::vector<ChartRecord> all_charts() const;
    std::vector<ScoreRecord> all_scores() const;
    bool find_by_md5(const std::string& md5, ChartRecord& out) const;
    // プレイの結果を記録する（ランプ・EX・BPは良いほうを残す）
    void save_score(const std::string& sha256, int clear, int exscore, int bp, int max_combo);
    bool get_score(const std::string& sha256, ScoreRecord& out) const;
    // LR2のスコアデータベース（LR2files/Database/Score/<名前>.db）を取り込む。取り込んだ件数を返す（失敗なら -1）。
    // 手元に無い譜面のスコアも MD5 のまま残し、表示するときに BMjoy の記録と合わせて良いほうを使う
    int import_lr2_scores(const std::string& lr2_score_db, std::string* error = nullptr);

private:
    sqlite3* db_ = nullptr;
};

// 1つの譜面ファイルを読んで、データベースに入れる内容を作る。DPなど対象外の譜面なら false
bool make_chart_record(const std::string& path, ChartRecord& out);

}  // namespace bmj
