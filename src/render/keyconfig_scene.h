// キーコンフィグ画面。割り当ての変更と、入力の確認（押されているボタン・軸の生の値）を同じ画面で行う
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "input/keyconfig.h"

namespace bmj {

// 閉じるまで動き続ける。変更は path に保存する
void run_keyconfig(SDL_Window* window, SDL_Renderer* r, KeyConfig& cfg, const std::string& path);

}  // namespace bmj
