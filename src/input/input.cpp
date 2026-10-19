#include "input/input.h"
#include "util/dev.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace bmj {

namespace {

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

// パッドの機器の番号。SDL の番号と Raw Input（win_rawhid）の番号の両方を入れる
struct PadTable {
    std::mutex mu;
    std::map<uint32_t, int> number;  // which → 番号
    int assign(uint32_t which) {
        auto it = number.find(which);
        if (it != number.end()) return it->second;
        int n = 1;
        while (std::any_of(number.begin(), number.end(), [&](const auto& p) { return p.second == n; })) ++n;
        number[which] = n;
        return n;
    }
    // SDL が知っているパッドを、SDL が並べた順に入れる。抜かれたものは外す（Raw Input の分はそのまま）
    void sync_sdl() {
        if (!SDL_WasInit(SDL_INIT_JOYSTICK)) return;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&n);
        for (auto it = number.begin(); it != number.end();) {
            bool sdl = it->first < kRawHidIdBase;
            bool alive = std::find(ids, ids + (ids ? n : 0), it->first) != ids + (ids ? n : 0);
            if (sdl && !alive) it = number.erase(it);
            else ++it;
        }
        for (int i = 0; ids && i < n; ++i) assign(ids[i]);
        SDL_free(ids);
    }
    static constexpr uint32_t kRawHidIdBase = 0x7f000000;  // win_rawhid.cpp と同じ
};

PadTable& pads() {
    static PadTable t;
    return t;
}

}  // namespace

int pad_number(uint32_t which) {
    PadTable& t = pads();
    std::lock_guard<std::mutex> lock(t.mu);
    auto it = t.number.find(which);
    if (it != t.number.end()) return it->second;
    t.sync_sdl();  // 先に SDL の並びで振っておく（押した順で番号が変わらないように）
    return t.assign(which);
}

void pad_forget(uint32_t which) {
    PadTable& t = pads();
    std::lock_guard<std::mutex> lock(t.mu);
    t.number.erase(which);
}

int pad_count() {
    PadTable& t = pads();
    std::lock_guard<std::mutex> lock(t.mu);
    t.sync_sdl();
    return static_cast<int>(t.number.size());
}

int capture_device(uint32_t which) {
    int n = pad_count() >= 2 ? pad_number(which) : 0;
    return n <= InputMapper::kPads ? n : 0;
}

uint32_t InputMapper::midi_event_type() {
    static const uint32_t type = SDL_RegisterEvents(1);
    return type;
}

void push_midi_event(int source, bool down, uint64_t ts) {
    uint32_t type = InputMapper::midi_event_type();
    if (!type || source < 0 || source >= kMidiSourceCount) return;
    SDL_Event e{};
    e.type = type;
    e.user.timestamp = ts;  // 判定はこの時刻で行う
    e.user.code = source;
    e.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(down ? 1 : 0));
    SDL_PushEvent(&e);
}

bool read_midi_event(const SDL_Event& e, int& source, bool& down) {
    uint32_t type = InputMapper::midi_event_type();
    if (!type || e.type != type) return false;
    source = e.user.code;
    down = reinterpret_cast<intptr_t>(e.user.data1) != 0;
    return source >= 0 && source < kMidiSourceCount;
}

int InputMapper::source_of(const Binding& b) {
    int base = kJoyBase;
    if (b.joystick()) {
        if (b.device < 0 || b.device > kPads) return -1;
        base += b.device * kJoyBlock;  // 0 = どのパッドでもよい、1〜 = その機器だけ
    }
    switch (b.type) {
    case Binding::Type::Key: return b.index >= 0 && b.index < kJoyBase ? b.index : -1;
    case Binding::Type::JoyButton: return b.index >= 0 && b.index < kJoyButtons ? base + b.index : -1;
    case Binding::Type::JoyAxis: return b.index >= 0 && b.index < kAxes ? base + kJoyAxisOffset + b.index * 2 + (b.dir > 0 ? 1 : 0) : -1;
    case Binding::Type::JoyHat: {
        int d = hat_dir_index(b.dir);
        return b.index >= 0 && b.index < kHats && d >= 0 ? base + kJoyHatOffset + b.index * 4 + d : -1;
    }
    case Binding::Type::Midi: return b.index >= 0 && b.index < kMidiSourceCount ? kMidiBase + b.index : -1;
    }
    return -1;
}

// パッドの入力は、どのパッドでもよい割り当てと、その機器だけの割り当ての両方に渡す
void InputMapper::joy_change(uint32_t which, int offset, bool down, uint64_t ts) {
    source_change(kJoyBase + offset, down, ts);
    int n = pad_number(which);
    if (n >= 1 && n <= kPads) source_change(kJoyBase + n * kJoyBlock + offset, down, ts);
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
    int midi = 0;
    bool midi_down = false;
    if (read_midi_event(e, midi, midi_down)) {
        source_change(kMidiBase + midi, midi_down, e.user.timestamp);
        return;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.repeat) return;
        if (e.key.scancode < kJoyBase) source_change(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.timestamp);
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        if (e.jbutton.button < kJoyButtons)
            joy_change(e.jbutton.which, e.jbutton.button, e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN, e.jbutton.timestamp);
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        int a = e.jaxis.axis;
        if (a == cfg_.analog_axis) {
            analog_motion(e.jaxis.which, e.jaxis.value, e.jaxis.timestamp);
            break;
        }
        if (a >= kAxes) break;
        // LR2は軸をアナログとして扱わず、しきい値を超えた向きのボタンとして読む
        joy_change(e.jaxis.which, kJoyAxisOffset + a * 2 + 1, e.jaxis.value > kAxisThreshold, e.jaxis.timestamp);
        joy_change(e.jaxis.which, kJoyAxisOffset + a * 2, e.jaxis.value < -kAxisThreshold, e.jaxis.timestamp);
        break;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        int h = e.jhat.hat;
        if (h >= kHats) break;
        Uint8 v = e.jhat.value;
        joy_change(e.jhat.which, kJoyHatOffset + h * 4 + 0, v & SDL_HAT_UP, e.jhat.timestamp);
        joy_change(e.jhat.which, kJoyHatOffset + h * 4 + 1, v & SDL_HAT_DOWN, e.jhat.timestamp);
        joy_change(e.jhat.which, kJoyHatOffset + h * 4 + 2, v & SDL_HAT_LEFT, e.jhat.timestamp);
        joy_change(e.jhat.which, kJoyHatOffset + h * 4 + 3, v & SDL_HAT_RIGHT, e.jhat.timestamp);
        break;
    }
    case SDL_EVENT_JOYSTICK_ADDED: pad_number(e.jdevice.which); break;
    case SDL_EVENT_JOYSTICK_REMOVED: pad_forget(e.jdevice.which); break;
    default: break;
    }
}

// アナログ皿：値が動いた向きを押し、反対に回したら押し直す。止まったら collect() で離す
void InputMapper::analog_motion(uint32_t which, int16_t value, uint64_t ts) {
    auto [it, first] = analog_last_.try_emplace(which, value);
    if (first) return;
    int d = static_cast<int>(value) - it->second;
    // 1周で値が端から端へ戻るときの折り返し
    if (d > 32768) d -= 65536;
    if (d < -32768) d += 65536;
    it->second = value;
    if (d == 0) return;
    int dir = (d > 0) == !cfg_.analog_invert ? 1 : -1;
    static const bool debug = dev_env("BMJ_DEBUG_INPUT") != nullptr;
    if (debug && dir != analog_dir_) std::printf("皿: 値 %d（差 %d）で向き %d → %d\n", value, d, analog_dir_, dir);
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
        if (ts < s.press_ns + min_hold_ns_) {
            if (std::none_of(pending_release_.begin(), pending_release_.end(),
                             [&](const Pending& p) { return p.source == source; }))
                pending_release_.push_back({s.press_ns + min_hold_ns_, source});
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

bool BindingCapture::take(const SDL_Event& e, Binding& out, int analog_axis) {
    auto device_of = capture_device;  // パッドが 2 台以上なら、押した機器だけの割り当てにする
    int midi = 0;
    bool down = false;
    if (read_midi_event(e, midi, down)) {
        if (!down) return false;
        out = {Binding::Type::Midi, midi, 0};
        return true;
    }
    switch (e.type) {
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        out = {Binding::Type::JoyButton, e.jbutton.button, 0, device_of(e.jbutton.which)};
        return true;
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        int v = e.jhat.value;
        if (!v) return false;
        int dir = v & SDL_HAT_UP ? SDL_HAT_UP : v & SDL_HAT_DOWN ? SDL_HAT_DOWN : v & SDL_HAT_LEFT ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
        out = {Binding::Type::JoyHat, e.jhat.hat, dir, device_of(e.jhat.which)};
        return true;
    }
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        int a = e.jaxis.axis;
        auto [it, first] = axis_prev_.try_emplace({e.jaxis.which, a}, static_cast<int16_t>(0));
        int16_t prev = it->second;
        it->second = e.jaxis.value;
        if (a == analog_axis || std::abs(e.jaxis.value) <= 24000) return false;
        if (wait_center && (first || std::abs(prev) >= 16000)) return false;
        out = {Binding::Type::JoyAxis, a, e.jaxis.value > 0 ? 1 : -1, device_of(e.jaxis.which)};
        return true;
    }
    default: return false;
    }
}

}  // namespace bmj
