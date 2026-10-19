// キーコンフィグ画面（スキンの型 8）の状態と、スキンに渡す値。描画と入力の受け取りは keyconfig_skin_scene。
// 流れは LR2 と同じ：項目（鍵盤・皿・START・SELECT）を選ぶ → スロットを選ぶ → 割り当てる入力を押す
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "input/keyconfig.h"

namespace bmj {

// LR2 のキーコンフィグの項目の番号（ボタン 101〜139 の下 2 桁）→ BMjoy のボタン。BMjoy に無い項目（2P）は -1。
//   1〜7 = 1P の鍵盤、10 / 11 = 皿の 2 方向（10 = scratch_a、11 = scratch_b）、12 = START、13 = SELECT
int keyconfig_item_button(int item);
// ボタン 140 で前後に送る順（7 鍵モード）。n 番目の項目の番号（0 は「選んでいない」）。BMjoy では 1P の分だけ
constexpr int kKeyconfigOrder[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13};
constexpr int kKeyconfigOrderCount = static_cast<int>(sizeof(kKeyconfigOrder) / sizeof(kKeyconfigOrder[0]));

// 割り当てられないキー（ESC・Enter・矢印・F1〜F12。画面の操作に使う）
bool keyconfig_reserved(SDL_Scancode sc);

class KeyconfigEditor {
public:
    explicit KeyconfigEditor(KeyConfig& cfg) : cfg_(cfg) {}
    int item() const { return item_; }  // 選んでいる項目（0 なら無し）
    int slot() const { return slot_; }  // 選んでいるスロット（-1 なら無し）
    // スキンのボタン（101〜143・150〜157）を押した。dir は +1 / -1。受け付けたら true（音を鳴らす）
    bool click(int type, int dir);
    // 選んでいるスロットに入力を割り当てる。項目とスロットを選んでいなければ何もせず false
    bool assign(const Binding& b);
    // 選んでいるスロットを空ける（Delete）。項目とスロットを選んでいなければ false
    bool erase();
    void reset_default();  // F1：既定の割り当てに戻す（アナログ皿の設定は残す）
    void clear_all();      // F2：全部消す

    // スキンに渡す値（キーコンフィグの番号だけ。ほかは呼び出し側が答える）。答えたら true
    bool option(int op, bool& v) const;
    bool text(int id, std::string& s) const;
    bool button(int type, int& v) const;

private:
    KeyConfig& cfg_;
    // 入ったときは 1 鍵を選び、スロットは選んでいない（LR2 と同じ）
    int item_ = 1, slot_ = -1;
};

}  // namespace bmj
