// キーコンフィグ画面。割り当ての変更と、入力の確認（押されているボタン・軸の生の値）を同じ画面で行う
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>

#include "input/keyconfig.h"

namespace bmj {

// 閉じるまで動き続ける。変更は path に保存する
void run_keyconfig(SDL_Window* window, SDL_Renderer* r, KeyConfig& cfg, const std::string& path);

class TextCache;
// 「1鍵を押してください」…「皿を回してください」と順に聞いて割り当てる（選曲画面から呼ぶ）。
// 押した入力がコントローラーならコントローラーの割り当てだけを、キーボードならキーボードの割り当てだけを入れ替える。
// 最後まで行ったら path に保存して true、途中でやめたら何も変えずに false
bool run_key_wizard(SDL_Renderer* r, TextCache& text, KeyConfig& cfg, const std::string& path);

struct App;
struct SelectContext;
// 選曲画面のボタン 13。キーコンフィグのスキン（型 8）があれば、選曲画面をフェードアウトさせ（タイマー 2）、
// スキンの画面を出して、戻ったら割り当てを読み直す。スキンが無ければ run_key_wizard を出す。
// sctx・scene_ms は選曲画面のもの（フェードアウトを描くのに使う）。アプリを閉じるなら false。
// 中身は app/keyconfig_skin_scene.cpp
bool open_keyconfig_from_select(App& app, TextCache& text, SelectContext& sctx, const std::function<double()>& scene_ms);

}  // namespace bmj
