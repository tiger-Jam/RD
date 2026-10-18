// アプリの設定（ハイスピードや判定タイミング、スキンなど）。起動のたびに読み、終了時に書き戻す
#pragma once

#include <map>
#include <string>
#include <vector>

namespace bmj {

struct AppConfig {
    std::string lr2_root;          // LR2のフォルダ（LR2files があるところ）
    std::string play_skin;         // プレイスキンの .lr2skin（空なら LR2 の既定）
    std::string result_skin;       // リザルトスキンの .lr2skin（空なら LR2 の既定）
    std::string skin_res;          // "", "sd", "hd", "fhd"
    std::map<std::string, std::vector<int>> skin_custom;  // スキンのパス → カスタマイズの選択
    std::vector<std::string> song_dirs;  // 曲フォルダ
    std::vector<std::string> tables;     // 難易度表のURL（アプリには同梱しない。ユーザーが登録する）
    int hi_speed = 100;            // LR2のハイスピ（100=1.00倍）
    int judge_timing = 0;          // LR2の判定タイミング（描画だけをずらす）
    int gauge = 0;                 // GaugeType
    bool gas = false;
    // fps：0 はモニタのリフレッシュレート、-1 は上限なし。垂直同期は既定で使わない
    // （OpenLR2と同じく、描画を待つ間も入力と判定を回すため）
    int fps = 0;
    bool vsync = false;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// 設定ファイルの置き場所（macOS なら ~/Library/Application Support/BMjoy/config.txt）
std::string config_path();
// 曲データベースの置き場所（同じフォルダの songs.db）
std::string song_db_path();
// 難易度表を手元に保存しておくフォルダ
std::string table_cache_dir();

}  // namespace bmj
