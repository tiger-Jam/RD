// LR2 のカスタムフォルダ（.lr2folder と folderinfo.txt）。#COMMAND は LR2 の曲データベースへの SQL の WHERE 句なので、
// BMjoy の譜面と記録から LR2 と同じ形の表（song・score）をメモリ上に作り、そこで動かす（LR2: songmanage:725 LoadFolderDataFromDB）
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/song_db.h"

namespace bmj {

struct CustomFolderDef {
    std::string title, category, command;
    int max_tracks = 0;  // #MAXTRACKS（0 は制限なし）
};

// カスタムフォルダの置き場の直下の 1 つ。.lr2folder なら 1 つのフォルダ、ディレクトリならその中の .lr2folder をまとめたもの
struct CustomFolderNode {
    std::string title;
    bool group = false;
    std::vector<CustomFolderDef> defs;  // group でなければ 1 つ
};

bool parse_lr2folder(const std::string& path, CustomFolderDef& out);
// 置き場を読む。難易度表の管理ツールが作ったもの（playlist_entry を見るもの）と、BMjoy が自分で持つもの
// （NEW SONG・コースなど __ で囲んだ特別な語のもの）は読まない
std::vector<CustomFolderNode> load_custom_folders(const std::string& dir);

// フォルダの中身を集める表。favorites は md5 → 1 = FAVORITE 2 = IGNORE
class CustomFolderQuery {
public:
    ~CustomFolderQuery();
    bool build(const std::vector<ChartRecord>& charts, const std::unordered_map<std::string, ScoreRecord>& scores,
               const std::unordered_map<std::string, int>& favorites);
    // 当てはまる譜面の md5（SQL の並び。ORDER BY が無ければ表の並び）。IGNORE の譜面は入れない。
    // SQL が読めなければ空を返し、error に理由を入れる
    std::vector<std::string> run(const CustomFolderDef& def, std::string* error = nullptr) const;

private:
    struct sqlite3* db_ = nullptr;
};

}  // namespace bmj
