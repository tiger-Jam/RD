// LR2 の形の曲データベースとスコアを書き出す（外部ツール：BeMusicSeeker・スコアを見るツールなどのため）。
// RD 自身は songs.db を使い、ここで書くものは読まない。
//   LR2files/Database/song.db           song・folder の表だけを書き直す（ほかのツールが足した表はそのまま残す）
//   LR2files/Database/Score/<名前>.db    score・player の表を書き直す
// 文字は UTF-8（手元の LR2 の song.db と同じ）。LR2 本体が作る照合用の値（scorehash）は作れないので空
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/song_db.h"

namespace rd {

// LR2 のフォルダの番号：フォルダのパス（区切りは逆スラッシュ、末尾にも付ける）を Shift_JIS にして、終わりの NUL まで含めた CRC32
// （0x80 以上のバイトは符号付きとして混ぜる。16 進、先頭の 0 は付けない）
std::string lr2_folder_crc(const std::string& dir_utf8);
// 登録した曲フォルダ（一番上）の親の番号
inline constexpr const char* kLr2RootParent = "e2977170";

// song.db の song・folder の表を、charts で書き直す。roots は登録した曲フォルダ
bool export_lr2_song_db(const std::string& path, const std::vector<ChartRecord>& charts, const std::vector<std::string>& roots,
                        std::string* error = nullptr);
// LR2 から引き継いだもの（プレイヤーの情報と、コースの記録の元の鍵）
struct Lr2Carry {
    std::map<std::string, std::string> player;               // LR2 の player 表の欄 → 値
    std::unordered_map<std::string, std::string> course_keys;  // RD のコースの鍵 → LR2 の鍵
};
// Score/<player>.db の score・player の表を書き直す。scores は LR2 の LN・MOD なしの記録（キーは sha256、コースは RD のコースの鍵）。
// player の表は、引き継いだ情報（ID・名前・段位など）に、stats の累計を入れたもの
bool export_lr2_scores(const std::string& path, const std::string& player, const std::vector<ChartRecord>& charts,
                       const std::vector<ScoreRecord>& scores, const PlayerStats& stats, const Lr2Carry& carry,
                       std::string* error = nullptr);
// 曲データベースから、database_dir（LR2files/Database）の下に書く。songs が偽ならスコアだけ
// player が空なら、引き継いだ LR2 の ID（無ければ RD）
bool export_lr2_files(const SongDb& db, const std::string& database_dir, const std::vector<std::string>& roots,
                      std::string player, bool songs, std::string* error = nullptr);

}  // namespace rd
