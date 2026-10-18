#include "input/input.h"

#include <algorithm>
#include <cstdlib>

namespace bmj {

namespace {

constexpr uint64_t kMinHoldNs = 16'000'000;  // LR2: sys_inputinterval の既定値16ms
constexpr int16_t kAxisThreshold = 16384;    // 軸を方向ボタンとみなすしきい値

int hat_dir_index(int dir) {
    switch (dir) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_DOWN: return 1;
    case SDL_HAT_LEFT: return 2;
    case SDL_HAT_RIGHT: return 3;
    default: return -1;
    }
}

}  // namespace

int InputMapper::source_of(const Binding& b) {
    switch (b.type) {
    case Binding::Type::Key: return b.index >= 0 && b.index < kJoyBase ? b.index : -1;
    case Binding::Type::JoyButton: return b.index >= 0 && b.index < kJoyButtons ? kJoyBase + b.index : -1;
    case Binding::Type::JoyAxis: return b.index >= 0 && b.index < kAxes ? kAxisBase + b.index * 2 + (b.dir > 0 ? 1 : 0) : -1;
    case Binding::Type::JoyHat: {
        int d = hat_dir_index(b.dir);
        return b.index >= 0 && b.index < kHats && d >= 0 ? kHatBase + b.index * 4 + d : -1;
    }
    }
    return -1;
}

InputMapper::InputMapper(const KeyConfig& cfg) : cfg_(cfg) {
    for (int button = 0; button < kButtonCount; ++button)
        for (const Binding& b : cfg.buttons[button]) {
            int s = source_of(b);
            if (s >= 0) sources_[s].buttons.push_back(button);
        }
    if (cfg.analog_axis >= 0) {
        sources_[kAnalogBase].buttons.push_back(kScratchA);
        sources_[kAnalogBase + 1].buttons.push_back(kScratchB);
    }
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
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        int a = e.jaxis.axis;
        if (a == cfg_.analog_axis) {
            analog_motion(e.jaxis.value, e.jaxis.timestamp);
            break;
        }
        if (a >= kAxes) break;
        // LR2は軸をアナログとして扱わず、しきい値を超えた向きのボタンとして読む
        axis_[a] = e.jaxis.value;
        source_change(kAxisBase + a * 2 + 1, e.jaxis.value > kAxisThreshold, e.jaxis.timestamp);
        source_change(kAxisBase + a * 2, e.jaxis.value < -kAxisThreshold, e.jaxis.timestamp);
        break;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        int h = e.jhat.hat;
        if (h >= kHats) break;
        Uint8 v = e.jhat.value;
        source_change(kHatBase + h * 4 + 0, v & SDL_HAT_UP, e.jhat.timestamp);
        source_change(kHatBase + h * 4 + 1, v & SDL_HAT_DOWN, e.jhat.timestamp);
        source_change(kHatBase + h * 4 + 2, v & SDL_HAT_LEFT, e.jhat.timestamp);
        source_change(kHatBase + h * 4 + 3, v & SDL_HAT_RIGHT, e.jhat.timestamp);
        break;
    }
    default: break;
    }
}

// アナログ皿：値が動いた向きを押し、反対に回したら押し直す。止まったら collect() で離す
void InputMapper::analog_motion(int16_t value, uint64_t ts) {
    if (!analog_init_) {
        analog_init_ = true;
        analog_last_ = value;
        return;
    }
    int d = static_cast<int>(value) - analog_last_;
    // 1周で値が端から端へ戻るときの折り返し
    if (d > 32768) d -= 65536;
    if (d < -32768) d += 65536;
    analog_last_ = value;
    if (d == 0) return;
    int dir = (d > 0) == !cfg_.analog_invert ? 1 : -1;
    analog_move_ns_ = ts;
    if (dir == analog_dir_) return;
    if (analog_dir_ != 0) source_change(kAnalogBase + (analog_dir_ > 0 ? 1 : 0), false, ts);
    analog_dir_ = dir;
    source_change(kAnalogBase + (dir > 0 ? 1 : 0), true, ts);
}

void InputMapper::source_change(int source, bool down, uint64_t ts) {
    Source& s = sources_[source];
    if (s.buttons.empty()) return;
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
        for (int b : s.buttons) apply_button(b, true, ts);
    } else {
        if (!s.down) return;
        if (ts < s.press_ns + kMinHoldNs) {
            if (std::none_of(pending_release_.begin(), pending_release_.end(),
                             [&](const Pending& p) { return p.source == source; }))
                pending_release_.push_back({s.press_ns + kMinHoldNs, source});
            return;
        }
        s.down = false;
        for (int b : s.buttons) apply_button(b, false, ts);
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
    // アナログ皿が止まって一定時間経ったら離す
    if (analog_dir_ != 0 && now_ns >= analog_move_ns_ + static_cast<uint64_t>(cfg_.analog_release_ms) * 1000000ull) {
        source_change(kAnalogBase + (analog_dir_ > 0 ? 1 : 0), false,
                      analog_move_ns_ + static_cast<uint64_t>(cfg_.analog_release_ms) * 1000000ull);
        analog_dir_ = 0;
    }
    for (auto it = pending_release_.begin(); it != pending_release_.end();) {
        if (it->due_ns <= now_ns) {
            Source& s = sources_[it->source];
            s.down = false;
            uint64_t due = it->due_ns;
            it = pending_release_.erase(it);
            for (int b : s.buttons) apply_button(b, false, due);
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
