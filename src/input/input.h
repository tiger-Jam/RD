// 入力（仕様書6章）。キーボード・ジョイスティックの入力を、時刻付きのレーン入力に変換する。
// 挙動はLR2に合わせている（参照: OpenLR2 LR2/En_input.cpp）
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
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

    // 入力元の番号：0..511 キーボード（SDLのスキャンコード）、512以降 ジョイスティック
    static constexpr int kJoyBase = 512;
    static constexpr int kJoyButtons = 64;
    static constexpr int kAxisBase = kJoyBase + kJoyButtons;  // 軸 i の + は kAxisBase+2i+1、- は +2i
    static constexpr int kAxes = 8;
    static constexpr int kHatBase = kAxisBase + kAxes * 2;    // ハット i の向き d は kHatBase+4i+d
    static constexpr int kHats = 4;
    static constexpr int kAnalogBase = kHatBase + kHats * 4;  // アナログ皿の2方向
    static constexpr int kSourceCount = kAnalogBase + 2;
    static int source_of(const Binding& b);

private:
    struct Source {
        std::vector<int> buttons;
        bool down = false;
        uint64_t press_ns = 0;
    };
    void source_change(int source, bool down, uint64_t ts);
    void apply_button(int button, bool down, uint64_t ts);
    void analog_motion(uint32_t which, int16_t value, uint64_t ts);

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
    std::array<int16_t, kAxes> axis_{};
    // アナログ皿の状態
    // 前回の値は機器ごとに持つ（同じ番号の軸を持つ機器が2台あると、交互に届いて向きが乱れる）
    std::unordered_map<uint32_t, int16_t> analog_last_;
    uint64_t min_hold_ns_ = 16'000'000;
    int analog_dir_ = 0;  // 0=止まっている, 1/-1=回している向き
    uint64_t analog_move_ns_ = 0;
};

}  // namespace bmj
