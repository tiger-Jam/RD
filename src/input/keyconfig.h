// キーコンフィグ。ボタンごとに、キーボードのキーやジョイスティックの入力を複数割り当てられる。
// 既定値はLR2と同じ（LR2files/Config/keyconfig_def.xml）
#pragma once

#include <array>
#include <string>
#include <vector>

namespace bmj {

// LR2のボタン番号に対応（鍵盤1〜7、皿の2方向、START、SELECT）
enum Button : int {
    kKey1 = 0, kKey7 = 6, kScratchA = 7, kScratchB = 8, kStart = 9, kSelect = 10, kButtonCount = 11
};

struct Binding {
    enum class Type { Key, JoyButton, JoyAxis, JoyHat } type = Type::Key;
    int index = 0;  // キーならSDLのスキャンコード、ジョイスティックならボタン・軸・ハットの番号
    int dir = 0;    // 軸なら +1 / -1、ハットなら SDL_HAT_UP などのビット
    bool operator==(const Binding& o) const { return type == o.type && index == o.index && dir == o.dir; }
};

struct KeyConfig {
    std::array<std::vector<Binding>, kButtonCount> buttons;
    // アナログ皿（BMjoyの拡張。LR2は軸を方向ボタンとしてしか読まない）。-1 なら使わない
    int analog_axis = -1;
    bool analog_invert = false;
    int analog_release_ms = 100;  // 皿が止まってから離したとみなすまでの時間

    static KeyConfig lr2_default();
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// キーコンフィグ画面のスロット（1 つのボタンに 8 個。LR2 と同じ数）。
// スロット n は buttons[ボタン] の n 番目の割り当て。空きは詰めて持つので、空いたスロットに入れると末尾に足す
// （= 一番前の空きスロットに入る）。スロットを消すと後ろの割り当てが 1 つずつ前へ詰まる
constexpr int kKeySlots = 8;
// スロットに割り当てる。同じボタンのほかのスロットに同じ入力があれば、そちらを外して移す
void set_slot(KeyConfig& cfg, int button, int slot, const Binding& b);
// スロットを空ける（空なら何もしない）
void clear_slot(KeyConfig& cfg, int button, int slot);
// 鍵盤・皿・START・SELECT の割り当てを全部消す（アナログ皿の設定は残す）
void clear_bindings(KeyConfig& cfg);
// キーコンフィグ画面に出す入力の名前（キー名は大文字、パッドは JOY:BUTTON1 など。番号は 1 から）
std::string binding_label(const Binding& b);

const char* button_name(int button);
std::string describe(const Binding& b);
// 設定ファイルの置き場所（macOS なら ~/Library/Application Support/BMjoy/keyconfig.txt）
std::string keyconfig_path();

}  // namespace bmj
