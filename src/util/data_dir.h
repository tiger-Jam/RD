// BMjoy の置き場。LR2 と同じく、実行ファイルの横の LR2files にまとめる（フォルダごと別の PC へ持っていける）。
//   LR2files/Config     設定（config.txt）・キーコンフィグ（keyconfig.txt）
//   LR2files/Database   曲データベースと記録（songs.db）・LR2 から引き継いだ段位（lr2_courses.txt）・難易度表の手元の版（Table/）
//   LR2files/Replay     リプレイ（*.bjr）・全プレーの入力の記録（InputLog/）
//   LR2files/Mods       自分で入れた MOD・ModData（MOD が書くファイル）
// 実行ファイルのフォルダに書けないときだけ、OS の設定フォルダ（前の置き場）を使う
#pragma once

#include <string>

namespace bmj {

// 実行ファイルのフォルダ（末尾に区切りが付く）
std::string base_dir();
// データの置き場（末尾に区切りが付く）。ふつうは <実行ファイルのフォルダ>/LR2files/
std::string data_dir();
// 前の置き場（OS の設定フォルダ）にだけ設定があれば、新しい置き場へ写す（前の置き場は消さずに残す）。
// 起動のはじめに 1 回呼ぶ。写したら true
bool migrate_old_data();
// 設定に書くパス：実行ファイルのフォルダの下なら、そこからの相対パスにする（別の PC へ持っていっても使える）
std::string to_portable(const std::string& path);
// 設定から読んだパス：相対パスなら、実行ファイルのフォルダからの絶対パスにする
std::string from_portable(const std::string& path);

}  // namespace bmj
