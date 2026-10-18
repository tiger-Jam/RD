// 入力（仕様書6章）。キーボード・ジョイスティックの入力を、時刻付きのレーン入力に変換する。
// 割り当ての既定値と挙動はLR2に合わせている（参照: LR2files/Config/keyconfig_def.xml, OpenLR2 LR2/En_input.cpp）
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <vector>

namespace bmj {

// LR2のボタン番号に対応（鍵盤1〜7、皿の2方向、START、SELECT）
enum Button : int {
    kKey1 = 0, kKey7 = 6, kScratchA = 7, kScratchB = 8, kStart = 9, kSelect = 10, kButtonCount = 11
};

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
    InputMapper();
    // SDLのイベントを1つ渡す。対応する入力があれば内部のキューに積む
    void handle(const SDL_Event& e);
    // now_ns までに確定した入力を取り出す（離す入力は、押してから16ms経つまで保留する）
    void collect(uint64_t now_ns, std::vector<LaneInput>& lanes, std::vector<ButtonInput>& buttons);
    bool button_held(int button) const { return held_count_[button] > 0; }

private:
    // 入力元（キーボードのキー、ジョイスティックのボタンや軸の向き）ごとの状態
    struct Source {
        int button = -1;
        bool down = false;
        uint64_t press_ns = 0;
    };
    void source_change(int source, bool down, uint64_t ts);
    void apply_button(int button, bool down, uint64_t ts);

    // 入力元の番号：0..511 キーボード（SDLのスキャンコード）、512以降 ジョイスティック
    static constexpr int kJoyBase = 512;
    static constexpr int kJoyButtons = 32;
    static constexpr int kSourceCount = kJoyBase + kJoyButtons + 4;
    static int joy_axis_source(int dir) { return kJoyBase + kJoyButtons + dir; }  // 0=下,1=左,2=右,3=上

    std::array<Source, kSourceCount> sources_{};
    std::array<int, kButtonCount> held_count_{};
    struct Pending {
        uint64_t due_ns;
        int source;
    };
    std::vector<Pending> pending_release_;
    std::vector<LaneInput> lane_out_;
    std::vector<ButtonInput> button_out_;
    std::array<int16_t, 2> axis_{};
};

}  // namespace bmj
