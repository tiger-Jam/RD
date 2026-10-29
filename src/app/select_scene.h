// 選曲画面（仕様書9章）。難易度表と曲フォルダを同じ「フォルダ」として並べる。
// 見た目はまだ仮のもの（選曲スキンの形式は未決）
#pragma once

#include <memory>

#include "app/app.h"

namespace rd {

// 選曲画面の下ごしらえ（曲の一覧づくり）。自分の曲データベースの接続を使うので、裏のスレッドで作ってよい。
// 起動の画面で「PUSH ANY KEY」を待っている間に作っておき、READY のあとすぐ選曲画面に入れるようにする
struct SelectPrep;
std::shared_ptr<SelectPrep> prepare_select(App& app);
// 選曲スキンを先に読んでおく（画面の処理から呼ぶ。run_select はもう一度は読まない）
void preload_select_skin(App& app);

// 閉じるまで動き続ける。選んだ譜面は run_play で遊び、終わったら戻ってくる。prep が無ければその場で作る
void run_select(App& app, std::shared_ptr<SelectPrep> prep = nullptr);

}  // namespace rd
