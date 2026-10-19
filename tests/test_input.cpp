#include <cstdio>
#include <filesystem>

#include "app/keyconfig_editor.h"
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

TEST(keyconfig_slots) {
    KeyConfig c;
    const Binding z{Binding::Type::Key, SDL_SCANCODE_Z, 0}, x{Binding::Type::Key, SDL_SCANCODE_X, 0};
    const Binding b3{Binding::Type::JoyButton, 3, 0}, up{Binding::Type::JoyAxis, 1, -1};
    auto& v = c.buttons[kKey1];
    // 空いたスロットに入れると詰めて末尾に足す
    set_slot(c, kKey1, 5, z);
    CHECK_EQ(v.size(), size_t(1));
    CHECK(v[0] == z);
    set_slot(c, kKey1, 1, x);
    set_slot(c, kKey1, 2, b3);
    CHECK_EQ(v.size(), size_t(3));
    // 埋まっているスロットは置き換える
    set_slot(c, kKey1, 1, up);
    CHECK(v[1] == up);
    CHECK_EQ(v.size(), size_t(3));
    // 同じ入力がほかのスロットにあれば移す（同じボタンに 2 つは持たない）
    set_slot(c, kKey1, 2, z);  // z: 0 → 2（b3 を置き換える）
    CHECK_EQ(v.size(), size_t(2));
    CHECK(v[0] == up);
    CHECK(v[1] == z);
    set_slot(c, kKey1, 0, z);
    CHECK_EQ(v.size(), size_t(1));
    CHECK(v[0] == z);
    // 8 個より後ろには入れない
    set_slot(c, kKey1, kKeySlots, x);
    CHECK_EQ(v.size(), size_t(1));
    // スロットを空けると後ろが詰まる
    set_slot(c, kKey1, 1, x);
    set_slot(c, kKey1, 2, b3);
    clear_slot(c, kKey1, 1);
    CHECK_EQ(v.size(), size_t(2));
    CHECK(v[1] == b3);
    clear_slot(c, kKey1, 7);  // 空きは何もしない
    CHECK_EQ(v.size(), size_t(2));
    // ほかのボタンに同じ入力があっても残す（LR2 と同じ）
    set_slot(c, kKey1 + 1, 0, z);
    CHECK(c.buttons[kKey1][0] == z);
    CHECK(c.buttons[kKey1 + 1][0] == z);
    c.analog_axis = 3;
    clear_bindings(c);
    for (const auto& w : c.buttons) CHECK(w.empty());
    CHECK_EQ(c.analog_axis, 3);
}

TEST(keyconfig_labels) {
    CHECK(binding_label({Binding::Type::Key, SDL_SCANCODE_Z, 0}) == "Z");
    CHECK(binding_label({Binding::Type::Key, SDL_SCANCODE_LSHIFT, 0}) == "LEFT SHIFT");
    CHECK(binding_label({Binding::Type::JoyButton, 3, 0}) == "JOY:BUTTON4");
    CHECK(binding_label({Binding::Type::JoyAxis, 1, 1}) == "JOY:DOWN");
    CHECK(binding_label({Binding::Type::JoyAxis, 0, -1}) == "JOY:LEFT");
    CHECK(binding_label({Binding::Type::JoyAxis, 2, 1}) == "JOY:AXIS3+");
    CHECK(binding_label({Binding::Type::JoyHat, 0, SDL_HAT_UP}) == "JOY:HAT1 UP");
}

TEST(keyconfig_editor_items) {
    // LR2 の項目の番号 → BMjoy のボタン
    CHECK_EQ(keyconfig_item_button(1), int(kKey1));
    CHECK_EQ(keyconfig_item_button(7), int(kKey7));
    CHECK_EQ(keyconfig_item_button(10), int(kScratchA));
    CHECK_EQ(keyconfig_item_button(11), int(kScratchB));
    CHECK_EQ(keyconfig_item_button(12), int(kStart));
    CHECK_EQ(keyconfig_item_button(13), int(kSelect));
    for (int i : {0, 8, 9, 14, 21, 27, 30, 33}) CHECK_EQ(keyconfig_item_button(i), -1);
    CHECK(keyconfig_reserved(SDL_SCANCODE_ESCAPE));
    CHECK(keyconfig_reserved(SDL_SCANCODE_RETURN));
    CHECK(keyconfig_reserved(SDL_SCANCODE_LEFT));
    CHECK(keyconfig_reserved(SDL_SCANCODE_F1));
    CHECK(keyconfig_reserved(SDL_SCANCODE_F12));
    CHECK(!keyconfig_reserved(SDL_SCANCODE_KP_ENTER));
    CHECK(!keyconfig_reserved(SDL_SCANCODE_DELETE));
    CHECK(!keyconfig_reserved(SDL_SCANCODE_1));

    KeyConfig c = KeyConfig::lr2_default();
    KeyconfigEditor ed(c);
    int v = -1;
    std::string s;
    // 入ったときは 1 鍵。スロットは選んでいないので、押しても割り当てない
    CHECK_EQ(ed.item(), 1);
    CHECK(ed.button(101, v) && v == 1);
    CHECK(ed.button(140, v) && v == 1);
    CHECK(!ed.assign({Binding::Type::Key, SDL_SCANCODE_A, 0}));
    CHECK(ed.text(40, s) && s == "Z");
    CHECK(ed.text(42, s) && s == "-");
    // 2P と鍵盤モードは押せない
    CHECK(!ed.click(121, 1));
    CHECK(!ed.click(143, 1));
    CHECK(!ed.click(141, 1));
    CHECK_EQ(ed.item(), 1);
    bool o = false;
    CHECK(ed.option(400, o) && o);
    CHECK(ed.option(401, o) && !o);
    CHECK(ed.option(402, o) && !o);
    // START を選んでスロット 2 に割り当てる（空きなので詰めて 2 番目に入る）
    CHECK(ed.click(112, 1));
    CHECK(ed.button(140, v) && v == 10);
    CHECK(ed.click(152, 1));
    CHECK(ed.button(152, v) && v == 1);
    CHECK(ed.assign({Binding::Type::Key, SDL_SCANCODE_A, 0}));
    CHECK_EQ(c.buttons[kStart].size(), size_t(3));
    CHECK(ed.text(42, s) && s == "A");
    CHECK(ed.erase());
    CHECK_EQ(c.buttons[kStart].size(), size_t(2));
    // ボタン 140 で前後に送る（SELECT の次は「無し」、その次は 1 鍵。2P は飛ばす）
    CHECK(ed.click(140, 1));
    CHECK_EQ(ed.item(), 13);
    CHECK(ed.click(140, 1));
    CHECK_EQ(ed.item(), 0);
    CHECK(ed.text(40, s) && s == "-");
    CHECK(!ed.assign({Binding::Type::Key, SDL_SCANCODE_A, 0}));
    CHECK(ed.click(140, -1));
    CHECK_EQ(ed.item(), 13);
    CHECK(ed.click(140, 1) && ed.click(140, 1));
    CHECK_EQ(ed.item(), 1);
    // 全部消す・既定に戻す
    c.analog_axis = 2;
    ed.clear_all();
    CHECK(c.buttons[kKey1].empty());
    ed.reset_default();
    CHECK(c.buttons[kKey1] == KeyConfig::lr2_default().buttons[kKey1]);
    CHECK_EQ(c.analog_axis, 2);
}

TEST(keyconfig_slots_save_load) {
    // スロットで変えた割り当て（START / SELECT を含む）が、保存して読み直しても同じ並びで残る
    KeyConfig c = KeyConfig::lr2_default();
    KeyconfigEditor ed(c);
    ed.click(113, 1);
    ed.click(150, 1);
    ed.assign({Binding::Type::JoyAxis, 2, -1});
    ed.click(157, 1);
    ed.assign({Binding::Type::Key, SDL_SCANCODE_BACKSPACE, 0});
    std::string path = (std::filesystem::temp_directory_path() / "bmj_keyconfig_slots_test.txt").string();
    CHECK(c.save(path));
    KeyConfig d;
    CHECK(d.load(path));
    for (int b = 0; b < kButtonCount; ++b) CHECK(d.buttons[b] == c.buttons[b]);
    CHECK(d.buttons[kSelect][0] == (Binding{Binding::Type::JoyAxis, 2, -1}));
    CHECK(d.buttons[kSelect].back() == (Binding{Binding::Type::Key, SDL_SCANCODE_BACKSPACE, 0}));
    std::remove(path.c_str());
}
