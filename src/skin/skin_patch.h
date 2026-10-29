// スキンの差分（skinpatch/）。配られているスキンのファイルは書き換えず、RD で足した表示（GAS・EX-HARD のゲージの名前、
// 判定・メトロノーム・7K モードの欄など）を、読むときに当てる。差分は元のファイルの SHA-256 を名前にした <ハッシュ>.patch で、
// 中身が同じファイルにだけ当たる（違う版のスキンには当てず、元のまま読む）。差分で足す絵は「rdpatch:<名前>」で、差分の置き場から読む
#pragma once

#include <string>
#include <vector>

namespace rd {

// 差分の置き場（ふつうは実行ファイルの横の skinpatch/）。空なら当てない
void set_skin_patch_dir(const std::string& dir);
const std::string& skin_patch_dir();

// bytes（スキンのファイルの中身）に当たる差分があれば、lines（lines_of で分けた行）に当てる。当てたら true。
// 差分に書いてある元の行が 1 つでも合わなければ、何も変えずに false
bool apply_skin_patch(const std::string& bytes, std::vector<std::string>& lines);

// 「rdpatch:<名前>」なら差分の置き場の中のパスを、そうでなければ空を返す
std::string skin_patch_file(const std::string& path);

}  // namespace rd
