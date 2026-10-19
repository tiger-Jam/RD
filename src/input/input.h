// 入力（仕様書6章）。キーボード・ジョイスティック・MIDI の入力を、時刻付きのレーン入力に変換する。
// 挙動はLR2に合わせている（参照: OpenLR2 LR2/En_input.cpp）
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "input/keyconfig.h"

namespace bmj {

struct LaneInput {
    uint64_t time_ns = 0;  // SDL_GetTicksNS() 基準
    int lane = 0;          // 0=皿, 1..7=鍵盤
    bool down = false;
};

struct ButtonInput {
    uint64_t time_ns = 0;
    int button = 0;
    bool down = false;
};

class InputMapper {
public:
    explicit InputMapper(const KeyConfig& cfg = KeyConfig::lr2_default());
    // SDLのイベントを1つ渡す。対応する入力があれば内部のキューに積む
    void handle(const SDL_Event& e);
    // now_ns までに確定した入力を取り出す（離す入力は、押してから16ms経つまで保留する）
    void collect(uint64_t now_ns, std::vector<LaneInput>& lanes, std::vector<ButtonInput>& buttons);
    bool button_held(int button) const { return held_count_[button] > 0; }
    // 押してから離すまでの最短の時間（LR2: 最低連打間隔 inputinterval。既定 16ms）
    void set_min_hold_ms(int ms) { min_hold_ns_ = static_cast<uint64_t>(std::max(0, ms)) * 1000000ull; }

    // 入力元の番号：0..511 キーボード（SDLのスキャンコード）、512以降 ジョイスティック。
    // ジョイスティックは 1 台分の並び（ボタン 64・軸 8 の上下・ハット 4 の 4 方向）を、
    // 「どのパッドでもよい」分と、機器の番号 1〜kPads の分だけ持つ。押すと両方が動く
    static constexpr int kJoyBase = 512;
    static constexpr int kJoyButtons = 64;
    static constexpr int kAxes = 8;
    static constexpr int kHats = 4;
    static constexpr int kJoyAxisOffset = kJoyButtons;                // 軸 i の + は +2i+1、- は +2i
    static constexpr int kJoyHatOffset = kJoyAxisOffset + kAxes * 2;  // ハット i の向き d は +4i+d
    static constexpr int kJoyBlock = kJoyHatOffset + kHats * 4;
    static constexpr int kPads = 8;
    static constexpr int kMidiBase = kJoyBase + kJoyBlock * (kPads + 1);
    static constexpr int kAnalogBase = kMidiBase + kMidiSourceCount;  // アナログ皿の2方向
    static constexpr int kSourceCount = kAnalogBase + 2;
    static int source_of(const Binding& b);
    // MIDI の入力を運ぶ SDL のイベントの種類（SDL_RegisterEvents で取る）。0 なら取れなかった
    static uint32_t midi_event_type();

private:
    struct Source {
        std::vector<int> buttons;
        bool down = false;
        uint64_t press_ns = 0;
    };
    void source_change(int source, bool down, uint64_t ts);
    void apply_button(int button, bool down, uint64_t ts);
    void analog_motion(uint32_t which, int16_t value, uint64_t ts);
    void joy_change(uint32_t which, int offset, bool down, uint64_t ts);

    KeyConfig cfg_;
    std::array<Source, kSourceCount> sources_{};
    std::array<int, kButtonCount> held_count_{};
    struct Pending {
        uint64_t due_ns;
        int source;
    };
    std::vector<Pending> pending_release_;
    std::vector<LaneInput> lane_out_;
    std::vector<ButtonInput> button_out_;
    // アナログ皿の状態
    // 前回の値は機器ごとに持つ（同じ番号の軸を持つ機器が2台あると、交互に届いて向きが乱れる）
    std::unordered_map<uint32_t, int16_t> analog_last_;
    uint64_t min_hold_ns_ = 16'000'000;
    int analog_dir_ = 0;  // 0=止まっている, 1/-1=回している向き
    uint64_t analog_move_ns_ = 0;
};

// MIDI の入力を SDL のイベントとして送る（どのスレッドから呼んでもよい）。ts は SDL_GetTicksNS() 基準。
// source は MidiSource（0〜127 はノート）
void push_midi_event(int source, bool down, uint64_t ts);
// MIDI のイベントなら中身を読んで true
bool read_midi_event(const SDL_Event& e, int& source, bool& down);

// パッドの機器の番号（1 から）。初めて見た機器に、空いている一番小さい番号を振る（起動時は OS が並べた順）。
// 抜くと番号が空き、次につないだ機器がそれを使う（同じ機器をつなぎ直せば、たいてい同じ番号に戻る）。
// どのスレッドから呼んでもよい
int pad_number(uint32_t which);
void pad_forget(uint32_t which);
// つながっているパッドの数（番号を振った分）
int pad_count();
// キーコンフィグで割り当てるときに付ける機器の番号。パッドが 2 台以上なら pad_number、1 台だけなら 0（どのパッドでもよい）
int capture_device(uint32_t which);

// キーコンフィグで押された入力（キーボード以外）を割り当てに変える。
// パッドが 2 台以上つながっていれば機器の番号を付け、1 台だけならどのパッドでもよい割り当てにする
class BindingCapture {
public:
    // 割り当てる入力なら out に入れて true。analog_axis はアナログ皿に使っている軸（方向として割り当てない）
    bool take(const SDL_Event& e, Binding& out, int analog_axis = -1);
    // 軸は、中央付近から端まで倒したときだけ割り当てる。はじめから倒れている軸を拾わないよう、最初の値は覚えるだけ
    bool wait_center = true;

private:
    std::map<std::pair<uint32_t, int>, int16_t> axis_prev_;
};

}  // namespace bmj
