// キーコンフィグ画面。割り当ての変更と、入力の確認（押されているボタン・軸の生の値）を同じ画面で行う
#pragma once

#include <SDL3/SDL.h>

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

}  // namespace bmj
