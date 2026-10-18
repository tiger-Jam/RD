#include "input/input.h"

#include <algorithm>

namespace bmj {

namespace {

constexpr uint64_t kMinHoldNs = 16'000'000;  // LR2: sys_inputinterval の既定値16ms
constexpr int16_t kAxisThreshold = 16384;    // 軸を方向ボタンとみなすしきい値

}  // namespace

InputMapper::InputMapper() {
    auto key = [&](SDL_Scancode sc, int button) { sources_[sc].button = button; };
    // LR2の既定（1P）：Z S X D C F V、皿は左Ctrl・スペース／左Shift、START=Q、SELECT=W
    const SDL_Scancode keys[7] = {SDL_SCANCODE_Z, SDL_SCANCODE_S, SDL_SCANCODE_X, SDL_SCANCODE_D,
                                  SDL_SCANCODE_C, SDL_SCANCODE_F, SDL_SCANCODE_V};
    for (int i = 0; i < 7; ++i) key(keys[i], kKey1 + i);
    key(SDL_SCANCODE_LCTRL, kScratchA);
    key(SDL_SCANCODE_SPACE, kScratchA);
    key(SDL_SCANCODE_LSHIFT, kScratchB);
    key(SDL_SCANCODE_Q, kStart);
    key(SDL_SCANCODE_W, kSelect);
    // ジョイスティック：LR2の BUTTON(n) はSDLのボタン n-1。鍵盤は B4 B7 B3 B8 B2 B5 と左、皿は下／上
    const int pad_keys[6] = {4, 7, 3, 8, 2, 5};
    for (int i = 0; i < 6; ++i) sources_[kJoyBase + pad_keys[i] - 1].button = kKey1 + i;
    sources_[joy_axis_source(1)].button = kKey1 + 6;  // 左
    sources_[joy_axis_source(0)].button = kScratchA;  // 下
    sources_[joy_axis_source(3)].button = kScratchB;  // 上
    sources_[kJoyBase + 9 - 1].button = kStart;
    sources_[kJoyBase + 10 - 1].button = kSelect;
}

void InputMapper::handle(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.repeat) return;
        if (e.key.scancode < kJoyBase) source_change(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.timestamp);
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        if (e.jbutton.button < kJoyButtons)
            source_change(kJoyBase + e.jbutton.button, e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN, e.jbutton.timestamp);
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        // LR2は軸をアナログとして扱わず、上下左右の4方向のボタンとして読む
        if (e.jaxis.axis < 2) {
            axis_[e.jaxis.axis] = e.jaxis.value;
            int16_t x = axis_[0], y = axis_[1];
            source_change(joy_axis_source(0), y > kAxisThreshold, e.jaxis.timestamp);
            source_change(joy_axis_source(3), y < -kAxisThreshold, e.jaxis.timestamp);
            source_change(joy_axis_source(1), x < -kAxisThreshold, e.jaxis.timestamp);
            source_change(joy_axis_source(2), x > kAxisThreshold, e.jaxis.timestamp);
        }
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        Uint8 h = e.jhat.value;
        source_change(joy_axis_source(0), h & SDL_HAT_DOWN, e.jhat.timestamp);
        source_change(joy_axis_source(3), h & SDL_HAT_UP, e.jhat.timestamp);
        source_change(joy_axis_source(1), h & SDL_HAT_LEFT, e.jhat.timestamp);
        source_change(joy_axis_source(2), h & SDL_HAT_RIGHT, e.jhat.timestamp);
        break;
    }
    default: break;
    }
}

void InputMapper::source_change(int source, bool down, uint64_t ts) {
    Source& s = sources_[source];
    if (s.button < 0) return;
    if (down) {
        // 保留中の「離す」を取り消す（16ms以内に押し直した）
        auto it = std::find_if(pending_release_.begin(), pending_release_.end(),
                               [&](const Pending& p) { return p.source == source; });
        if (it != pending_release_.end()) {
            pending_release_.erase(it);
            return;  // 押しっぱなし扱い
        }
        if (s.down) return;
        s.down = true;
        s.press_ns = ts;
        apply_button(s.button, true, ts);
    } else {
        if (!s.down) return;
        if (ts < s.press_ns + kMinHoldNs) {
            pending_release_.push_back({s.press_ns + kMinHoldNs, source});
            return;
        }
        s.down = false;
        apply_button(s.button, false, ts);
    }
}

void InputMapper::apply_button(int button, bool down, uint64_t ts) {
    int& count = held_count_[button];
    int before = count;
    count += down ? 1 : -1;
    if (count < 0) count = 0;
    button_out_.push_back({ts, button, down});

    if (button <= kKey7) {
        // 鍵盤：最初の入力元で押下、最後の入力元が離れたら離す
        if (down && before == 0) lane_out_.push_back({ts, button + 1, true});
        if (!down && count == 0) lane_out_.push_back({ts, button + 1, false});
    } else if (button == kScratchA || button == kScratchB) {
        // 皿：どちらかの向きが新しく入ったら押下（反対向きを押している最中でも）。両方離れたら離す
        int total = held_count_[kScratchA] + held_count_[kScratchB];
        if (down && before == 0) lane_out_.push_back({ts, 0, true});
        if (!down && total == 0) lane_out_.push_back({ts, 0, false});
    }
}

void InputMapper::collect(uint64_t now_ns, std::vector<LaneInput>& lanes, std::vector<ButtonInput>& buttons) {
    for (auto it = pending_release_.begin(); it != pending_release_.end();) {
        if (it->due_ns <= now_ns) {
            Source& s = sources_[it->source];
            s.down = false;
            uint64_t due = it->due_ns;
            it = pending_release_.erase(it);
            apply_button(s.button, false, due);
        } else {
            ++it;
        }
    }
    std::stable_sort(lane_out_.begin(), lane_out_.end(),
                     [](const LaneInput& a, const LaneInput& b) { return a.time_ns < b.time_ns; });
    lanes.insert(lanes.end(), lane_out_.begin(), lane_out_.end());
    buttons.insert(buttons.end(), button_out_.begin(), button_out_.end());
    lane_out_.clear();
    button_out_.clear();
}

}  // namespace bmj
