// LR2 からの引き継ぎ。LR2 のフォルダ（LR2files があるところ）を指定して、曲フォルダの登録・曲の情報・ランプとスコアを取り込む
#pragma once

#include <functional>
#include <string>

#include "app/config.h"
#include "db/song_db.h"

namespace bmj {

struct Lr2ImportResult {
    bool ok = false;
    std::string error;
    std::string player;      // LR2 のプレイヤー ID
    int song_dirs = 0;       // 登録した曲フォルダの数
    int charts = 0;          // 取り込んだ 7 鍵の譜面の数（仮の記録）
    int scores = 0;          // 取り込んだスコアの数
};

// LR2 の設定（LR2files/Config/config.xmh、無ければ config.xml）の曲フォルダを cfg.song_dirs にし、
// 曲の情報（LR2files/Database/song.db）とスコア（LR2files/Database/Score/<プレイヤー ID>.db）を db に取り込む。
// LR2 のファイルは読むだけで書き換えない
Lr2ImportResult import_from_lr2(const std::string& lr2_dir, AppConfig& cfg, SongDb& db,
                                const std::function<void(const std::string&)>& status = {});

}  // namespace bmj
