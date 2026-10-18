#include <cstdio>
#include <filesystem>

#include "input/input.h"
#include "input/keyconfig.h"
#include "test.h"

using namespace bmj;

static SDL_Event key_event(SDL_Scancode sc, bool down, uint64_t ms) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.timestamp = ms * 1000000ull;
    return e;
}

static SDL_Event axis_event(int axis, int16_t v, uint64_t ms) {
    SDL_Event e{};
    e.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    e.jaxis.axis = static_cast<Uint8>(axis);
    e.jaxis.value = v;
    e.jaxis.timestamp = ms * 1000000ull;
    return e;
}

TEST(input_default_keys) {
    InputMapper m;
    std::vector<LaneInput> lanes;
    std::vector<ButtonInput> buttons;
    m.handle(key_event(SDL_SCANCODE_Z, true, 1000));
    m.handle(key_event(SDL_SCANCODE_LSHIFT, true, 1001));
    m.collect(1002 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(2));
    CHECK_EQ(lanes[0].lane, 1);
    CHECK_EQ(lanes[1].lane, 0);
}

TEST(input_min_hold_16ms) {
    InputMapper m;
    std::vector<LaneInput> lanes;
    std::vector<ButtonInput> buttons;
    m.handle(key_event(SDL_SCANCODE_Z, true, 1000));
    m.handle(key_event(SDL_SCANCODE_Z, false, 1005));  // 5msで離した
    m.collect(1010 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(1));  // まだ離していない扱い
    m.collect(1020 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(2));
    CHECK(!lanes[1].down);
    CHECK_EQ(lanes[1].time_ns, 1016 * 1000000ull);  // 押してから16ms後に離した扱い
}

TEST(input_analog_scratch) {
    KeyConfig c = KeyConfig::lr2_default();
    c.analog_axis = 3;
    c.analog_release_ms = 100;
    InputMapper m(c);
    std::vector<LaneInput> lanes;
    std::vector<ButtonInput> buttons;
    m.handle(axis_event(3, 1000, 1000));  // 最初の値は基準にするだけ
    m.handle(axis_event(3, 1300, 1010));  // 回した → 押下
    m.handle(axis_event(3, 1600, 1020));
    m.collect(1050 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(1));
    CHECK(lanes[0].down && lanes[0].lane == 0);
    m.handle(axis_event(3, 1300, 1060));  // 逆に回した → 押し直し
    m.collect(1070 * 1000000ull, lanes, buttons);
    int presses = 0;
    for (auto& l : lanes) presses += l.down;
    CHECK_EQ(presses, 2);
    m.collect(1200 * 1000000ull, lanes, buttons);  // 止まって100ms → 離す
    CHECK(!lanes.back().down);
    CHECK_EQ(lanes.back().time_ns, 1160 * 1000000ull);
    // 端をまたいで回しても向きは変わらない
    InputMapper m2(c);
    lanes.clear();
    m2.handle(axis_event(3, 32000, 0));
    m2.handle(axis_event(3, -32000, 5));  // +方向に回って折り返した
    m2.handle(axis_event(3, -31000, 10));
    m2.collect(20 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(1));
}

TEST(keyconfig_roundtrip) {
    KeyConfig c = KeyConfig::lr2_default();
    c.buttons[kKey1 + 2].push_back({Binding::Type::JoyHat, 0, SDL_HAT_LEFT});
    c.analog_axis = 2;
    c.analog_invert = true;
    c.analog_release_ms = 70;
    std::string path = (std::filesystem::temp_directory_path() / "bmj_keyconfig_test.txt").string();
    CHECK(c.save(path));
    KeyConfig d;
    CHECK(d.load(path));
    for (int b = 0; b < kButtonCount; ++b) CHECK(d.buttons[b] == c.buttons[b]);
    CHECK_EQ(d.analog_axis, 2);
    CHECK(d.analog_invert);
    CHECK_EQ(d.analog_release_ms, 70);
    std::remove(path.c_str());
}
