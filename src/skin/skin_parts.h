// LR2 スキンを部品（フレーム・ノーツ・ボム・判定…）ごとに切り出して、絵と一覧を書き出す（開発用）。
// 切り出すだけで、スキンの中身は何も捨てない（番号・タイマー・op は一覧にそのまま残す）
#pragma once

#include <string>

#include "skin/lr2_skin.h"

namespace rd {

// out_dir に、部品ごとのフォルダの PNG（アニメーションはコマごと）と、それを並べて見る index.html を書く。
// 書けなければ false
bool export_skin_parts(const Lr2Skin& skin, const std::string& out_dir, std::string& error);

}  // namespace rd
