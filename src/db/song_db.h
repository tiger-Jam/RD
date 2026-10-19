// 曲データベース（仕様書8章）。LR2とは別ファイルで、最初からWALで運用する。
// 書き込みはまとめて1トランザクションで行う
#pragma once

#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <vector>

struct sqlite3;

namespace bmj {

struct ChartRecord {
    std::string path, folder;  // 譜面ファイルと、それがあるフォルダ
    std::string sha256, md5;
    std::string title, subtitle, artist, subartist, genre;
    int playlevel = 0, difficulty = 0;
    int notes = 0, ln = 0, rank = 2, total = 0;  // notes は種類を決めていない LN を 1 本 = 1 で数えた数
    int ln_free = 0;  // 譜面で種類を決めていない LN の数（CN・HCN で遊ぶと 1 本ぶん多く数える。記録も LN の種類ごとに分ける）
    double bpm_min = 0, bpm_max = 0, bpm_main = 0;
    int length_ms = 0;
    bool scratch = true;
    std::string banner, stagefile, backbmp;  // 譜面に書かれた画像のファイル名（譜面のフォルダからの相対）
    int64_t mtime = 0, size = 0;
};

struct ScoreRecord {
    std::string sha256;
    int clear = 0;  // LR2のランプ（0=NO PLAY, 1=FAILED, 2=EASY, 3=NORMAL, 4=HARD, 5=FULL COMBO）
    int exscore = 0, bp = -1, max_combo = 0;
    int playcount = 0, clearcount = 0;
    int64_t last_played = 0;
    std::array<int, 5> judge{};  // ベストの EX スコアを出したときの PGREAT, GREAT, GOOD, BAD, POOR の数
    uint32_t op_history = 0;     // クリアしたときのオプションの履歴（LR2: op_history のビット。スキンの op 118〜149）
};

// 1 回のプレイの結果（記録する値）
struct PlayRecord {
    int clear = 0, exscore = 0, bp = 0, max_combo = 0;
    std::array<int, 5> judge{};
    uint32_t op_history = 0;
    // コースの途中のステージ（LR2 と同じく、プレイ回数・クリア回数・プレイヤーの累計は増やさず、ランプはフルコンボのときだけ上げる）
    bool course_stage = false;
    int ln_mode = 0;  // 記録を分ける LN の種類（種類を決めていない LN が無い譜面は 0）
};

// プレイヤーの累計（LR2: player 表。スキンの数値 30〜37）
struct PlayerStats {
    int playcount = 0, clear = 0, fail = 0;
    std::array<int64_t, 5> judge{};
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
                   const std::function<void(int done, int total, const std::string& path)>& progress = nullptr);

    std::vector<ChartRecord> all_charts() const;
    // 曲の一覧を消す（スコアは残す）。次の読み直しで全部登録し直す（LR2: 曲データベースの再作成）
    bool clear_charts();
    // ln_mode：LN の種類（種類を決めていない LN のある譜面だけ、その種類の記録を読む）
    std::vector<ScoreRecord> all_scores(int ln_mode = 0) const;
    bool find_by_md5(const std::string& md5, ChartRecord& out) const;
    // プレイの結果を記録する（ランプ・EX・BPは良いほうを残す）
    void save_score(const std::string& sha256, int clear, int exscore, int bp, int max_combo);
    void save_score(const std::string& sha256, const PlayRecord& r);
    PlayerStats player_stats() const;
    bool get_score(const std::string& sha256, ScoreRecord& out, int ln_mode = 0) const;
    // LR2のスコアデータベース（LR2files/Database/Score/<名前>.db）を取り込む。取り込んだ件数を返す（失敗なら -1）。
    // 手元に無い譜面のスコアも MD5 のまま残し、表示するときに BMjoy の記録と合わせて良いほうを使う
    int import_lr2_scores(const std::string& lr2_score_db, std::string* error = nullptr);
    // LR2 の曲データベース（song.db）から譜面の情報を取り込む。譜面ファイルは開かず、更新日時と大きさだけを読む。
    // 7 鍵の譜面は仮の記録（sha256 の欄が "md5:<md5>"）として入れ、それ以外の譜面は「読まない譜面」として覚える。
    // 取り込んだ 7 鍵の譜面の数を返す（失敗は -1）
    int import_lr2_songs(const std::string& lr2_song_db, std::string* error = nullptr,
                         const std::function<void(int, int)>& progress = {});
    // 仮の記録の譜面を、最大 limit 件だけ実際に読んで置き換える。読んだ数を返す（0 なら残り無し）
    int fill_provisional(int limit, int threads);
    int provisional_count() const;
    // LR2 の曲データベースに、難易度表の管理ツールが書いた難易度表の一覧（playlist 表）があれば、その URL を並び順に返す
    static std::vector<std::string> lr2_table_urls(const std::string& lr2_song_db);
    // 1 つの譜面を読み直す（仮の記録の譜面を遊んだあとなど）
    bool refresh_chart(const std::string& path);

private:
    sqlite3* db_ = nullptr;
};

// 1つの譜面ファイルを読んで、データベースに入れる内容を作る。DPなど対象外の譜面なら false
bool make_chart_record(const std::string& path, ChartRecord& out);

}  // namespace bmj
