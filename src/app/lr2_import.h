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
    int tables = 0;          // 新しく登録した難易度表の数
    int skins = 0;           // カスタマイズを引き継いだスキンの数
    int courses = 0;         // 引き継いだ段位・コースの数
};

// LR2 の設定（LR2files/Config/config.xmh、無ければ config.xml）の曲フォルダを cfg.song_dirs にし、
// LR2 の曲データベースにある難易度表（難易度表の管理ツールが書いたもの）の URL を cfg.tables に足し、
// 曲の情報（LR2files/Database/song.db）とスコア（LR2files/Database/Score/<プレイヤー ID>.db）を db に取り込む。
// skin_root（BMjoy がスキンを読むフォルダ。空なら lr2_dir）にあるスキンのうち、LR2 でカスタマイズしたもの
// （LR2files/SkinCustomize）は、その選択を cfg.skin_custom に写す。LR2 のファイルは読むだけで書き換えない
// LR2 の曲データベースに登録された段位（grade）とコース（expert）を lr2_courses_path() に写す。写した数（読めなければ -1）
int import_lr2_courses(const std::string& lr2_song_db);

Lr2ImportResult import_from_lr2(const std::string& lr2_dir, AppConfig& cfg, SongDb& db,
                                const std::function<void(const std::string&)>& status = {},
                                const std::string& skin_root = {});

// スキンのカスタマイズだけを引き継ぐ。引き継いだスキンの数を返す
int import_lr2_skin_custom(const std::string& lr2_dir, const std::string& skin_root, AppConfig& cfg);

struct AssetCopyResult {
    bool ok = false;
    std::string error;
    int files = 0;        // 写したファイルの数（同じものがすでにあれば数えない）
    int failed = 0;       // 写せなかったファイルの数
    int64_t bytes = 0;
};

// LR2 の素材（スキン・サウンドセット・BGM・壁紙・カスタムフォルダなど）を、BMjoy のフォルダ（dst_root。実行ファイルの横）の
// LR2files に写し、設定の中の素材の場所（cfg.lr2_root・各スキン・サウンドセット・スキンのカスタマイズと位置の調整の鍵・
// カスタムフォルダの置き場）を写した先に直す。LR2 の曲・記録・IR のデータ（Database・Ir・Replay など）は写さない。
// 写す元は cfg.lr2_root と、LR2 の LR2files の下にあるカスタムフォルダの置き場。元のファイルは読むだけ
AssetCopyResult copy_lr2_assets(AppConfig& cfg, const std::string& dst_root,
                                const std::function<void(const std::string&)>& status = {});

}  // namespace bmj
