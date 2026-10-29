// RD の置き場。LR2 の LR2files と同じように、実行ファイルの横の RDfiles にまとめる（フォルダごと別の PC へ持っていける）。
// スキン・サウンドセットなどの素材も同じところ（RDfiles/Theme など）に置く。スキンの中の「LR2files/…」はここに読み替える
//   RDfiles/Config     全員で共通の設定（config.txt）
//   RDfiles/Database 曲データベース（songs.db）・LR2 から引き継いだ段位（lr2_courses.txt）・難易度表の手元の版（Table/）
//   RDfiles/Player/<ID> プレイヤーごとのもの：設定（config.txt）・キーコンフィグ（keyconfig.txt）・記録（score.db）・
//                          リプレイ（Replay/）・MOD が書くファイル（ModData/）
// 実行ファイルのフォルダに書けないときだけ、OS の設定フォルダ（前の置き場）を使う
#pragma once

#include <string>
#include <vector>

namespace rd {

// 実行ファイルのフォルダ（末尾に区切りが付く）
std::string base_dir();
// データの置き場（末尾に区切りが付く）。ふつうは <実行ファイルのフォルダ>/RDfiles/（前の名前の LR2files なら最初に名前を変える）
std::string data_dir();
// データの置き場のフォルダの名前（RDfiles。名前を変えられなかったときは前の名前の LR2files）
std::string data_dir_name();
// 今のプレイヤーの ID（空ならプレイヤーを分けない。テストなど）
const std::string& player_id();
void set_player_id(const std::string& id);
// 今のプレイヤーの置き場（末尾に区切りが付く）：<データの置き場>/Player/<ID>/
std::string player_dir();
// プレイヤーの置き場の一覧（ID の順）
std::vector<std::string> list_players();
// プレイヤーの ID に使える名前か（空でなく、ファイル名に使えない文字を含まない）
bool valid_player_id(const std::string& id);
// 設定に書くパス：実行ファイルのフォルダの下なら、そこからの相対パスにする（別の PC へ持っていっても使える）
std::string to_portable(const std::string& path);
// 設定から読んだパス：相対パスなら、実行ファイルのフォルダからの絶対パスにする（前の名前の LR2files/… は今の置き場に読み替える）
std::string from_portable(const std::string& path);

}  // namespace rd
