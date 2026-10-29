#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "app/keyconfig_editor.h"
#include "input/input.h"
#include "input/keyconfig.h"
#include "input/midi.h"
#include "test.h"

using namespace rd;

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
    std::string path = (std::filesystem::temp_directory_path() / "rd_keyconfig_test.txt").string();
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
    // LR2 の項目の番号 → RD のボタン
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
    CHECK(ed.text(41, s) && s == "JOY:BUTTON4");
    CHECK(ed.text(42, s) && s == "MIDI NOTE:F3");
    CHECK(ed.text(43, s) && s == "MIDI NOTE:F4");
    CHECK(ed.text(44, s) && s == "-");
    // 2P と鍵盤モードは押せない
    CHECK(!ed.click(121, 1));
    CHECK(!ed.click(143, 1));
    CHECK(!ed.click(141, 1));
    CHECK_EQ(ed.item(), 1);
    bool o = false;
    CHECK(ed.option(400, o) && o);
    CHECK(ed.option(401, o) && !o);
    CHECK(ed.option(402, o) && !o);
    // START を選んでスロット 6 に割り当てる（空きなので詰めて 5 番目に入る）
    CHECK(ed.click(112, 1));
    CHECK(ed.button(140, v) && v == 10);
    CHECK(ed.click(155, 1));
    CHECK(ed.button(155, v) && v == 1);
    CHECK(ed.assign({Binding::Type::Key, SDL_SCANCODE_A, 0}));
    CHECK_EQ(c.buttons[kStart].size(), size_t(5));
    CHECK(ed.text(44, s) && s == "A");
    CHECK(ed.erase());  // スロット 6 は空きなので何もしない
    CHECK_EQ(c.buttons[kStart].size(), size_t(5));
    CHECK(ed.click(154, 1) && ed.erase());
    CHECK_EQ(c.buttons[kStart].size(), size_t(4));
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
    std::string path = (std::filesystem::temp_directory_path() / "rd_keyconfig_slots_test.txt").string();
    CHECK(c.save(path));
    KeyConfig d;
    CHECK(d.load(path));
    for (int b = 0; b < kButtonCount; ++b) CHECK(d.buttons[b] == c.buttons[b]);
    CHECK(d.buttons[kSelect][0] == (Binding{Binding::Type::JoyAxis, 2, -1}));
    CHECK(d.buttons[kSelect].back() == (Binding{Binding::Type::Key, SDL_SCANCODE_BACKSPACE, 0}));
    std::remove(path.c_str());
}

static SDL_Event button_event(uint32_t which, int button, bool down, uint64_t ms) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
    e.jbutton.which = which;
    e.jbutton.button = static_cast<Uint8>(button);
    e.jbutton.down = down;
    e.jbutton.timestamp = ms * 1000000ull;
    return e;
}

static SDL_Event midi_event(int source, bool down, uint64_t ms) {
    SDL_Event e{};
    e.type = InputMapper::midi_event_type();
    e.user.timestamp = ms * 1000000ull;
    e.user.code = source;
    e.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(down ? 1 : 0));
    return e;
}

TEST(keyconfig_parse_format_new_kinds) {
    // 機器を決めたパッドと MIDI の書き方。書いたものを読み戻すと同じになる
    const Binding cases[] = {
        {Binding::Type::JoyButton, 3, 0, 2},           {Binding::Type::JoyAxis, 1, -1, 3},
        {Binding::Type::JoyHat, 0, SDL_HAT_LEFT, 1},   {Binding::Type::Midi, 60, 0},
        {Binding::Type::Midi, kMidiBendMinus, 0},      {Binding::Type::Midi, kMidiBendPlus, 0},
        {Binding::Type::Midi, kMidiPedal, 0},          {Binding::Type::JoyButton, 9, 0},
    };
    for (const Binding& b : cases) {
        Binding r;
        CHECK(parse_binding(describe(b), r));
        CHECK(r == b);
    }
    CHECK(describe({Binding::Type::JoyButton, 3, 0, 2}) == "joy2:button 3");
    CHECK(describe({Binding::Type::JoyButton, 3, 0}) == "joy:button 3");  // 前からの書き方のまま
    CHECK(describe({Binding::Type::JoyAxis, 1, -1, 3}) == "joy3:axis 1-");
    CHECK(describe({Binding::Type::Midi, 60, 0}) == "midi:note 60");
    CHECK(describe({Binding::Type::Midi, kMidiBendPlus, 0}) == "midi:bend +");
    CHECK(describe({Binding::Type::Midi, kMidiPedal, 0}) == "midi:pedal");
    Binding r;
    CHECK(parse_binding("joy:button 5", r) && r == (Binding{Binding::Type::JoyButton, 5, 0}) && r.device == 0);
    CHECK(parse_binding("joy12:hat 1 up", r) && r.device == 12 && r.type == Binding::Type::JoyHat);
    CHECK(!parse_binding("joy0:button 1", r));
    CHECK(!parse_binding("joyx:button 1", r));
    CHECK(!parse_binding("midi:note 128", r));
    CHECK(!parse_binding("midi:bend", r));
    CHECK(!parse_binding("midi:knob 3", r));
    // 同じボタン・別の機器は別の割り当て
    CHECK(!(Binding{Binding::Type::JoyButton, 3, 0, 1} == Binding{Binding::Type::JoyButton, 3, 0, 2}));
    CHECK(!(Binding{Binding::Type::JoyButton, 3, 0, 1} == Binding{Binding::Type::JoyButton, 3, 0}));
}

TEST(keyconfig_labels_device_midi) {
    CHECK(binding_label({Binding::Type::JoyButton, 3, 0, 2}) == "JOY2:BUTTON4");
    CHECK(binding_label({Binding::Type::JoyAxis, 0, -1, 1}) == "JOY1:LEFT");
    CHECK(binding_label({Binding::Type::JoyHat, 0, SDL_HAT_DOWN, 3}) == "JOY3:HAT1 DOWN");
    CHECK(binding_label({Binding::Type::Midi, 60, 0}) == "MIDI NOTE:C5");
    CHECK(binding_label({Binding::Type::Midi, 41, 0}) == "MIDI NOTE:F3");
    CHECK(binding_label({Binding::Type::Midi, 37, 0}) == "MIDI NOTE:C#3");
    CHECK(binding_label({Binding::Type::Midi, kMidiBendMinus, 0}) == "MIDI BEND:-");
    CHECK(binding_label({Binding::Type::Midi, kMidiBendPlus, 0}) == "MIDI BEND:+");
    CHECK(binding_label({Binding::Type::Midi, kMidiPedal, 0}) == "MIDI PEDAL");
}

TEST(keyconfig_default_midi) {
    // 既定の MIDI 鍵盤の割り当て
    KeyConfig c = KeyConfig::lr2_default();
    auto has = [&](int button, int src) {
        const auto& v = c.buttons[button];
        return std::find(v.begin(), v.end(), Binding{Binding::Type::Midi, src, 0}) != v.end();
    };
    for (int i = 0; i < 7; ++i) CHECK(has(kKey1 + i, 41 + i) && has(kKey1 + i, 53 + i));
    for (int n : {36, 38, 48, 50, int(kMidiBendMinus)}) CHECK(has(kScratchA, n));
    for (int n : {40, 52, int(kMidiBendPlus)}) CHECK(has(kScratchB, n));
    CHECK(has(kStart, 39) && has(kStart, 51));
    CHECK(has(kSelect, 37) && has(kSelect, 49));
    for (const auto& v : c.buttons) CHECK(v.size() <= size_t(kKeySlots));
}

TEST(input_device_specific_pad) {
    // 機器を決めた割り当てはその機器だけ、決めていない割り当てはどの機器でも効く
    KeyConfig c;
    c.buttons[kKey1].push_back({Binding::Type::JoyButton, 0, 0, 1});
    c.buttons[kKey1 + 1].push_back({Binding::Type::JoyButton, 0, 0, 2});
    c.buttons[kKey1 + 2].push_back({Binding::Type::JoyButton, 1, 0});
    const uint32_t a = 0x7f00a001, b = 0x7f00a002;  // Raw Input の番号の範囲（SDL の一覧と混ざらない）
    pad_forget(a), pad_forget(b);
    int na = pad_number(a), nb = pad_number(b);
    CHECK(na != nb);
    CHECK_EQ(pad_number(a), na);  // 何度聞いても同じ
    c.buttons[kKey1][0].device = na;
    c.buttons[kKey1 + 1][0].device = nb;
    InputMapper m(c);
    std::vector<LaneInput> lanes;
    std::vector<ButtonInput> buttons;
    m.handle(button_event(a, 0, true, 100));
    m.collect(101 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(1));
    CHECK(lanes.size() == 1 && lanes[0].lane == 1);
    m.handle(button_event(b, 0, true, 110));
    m.handle(button_event(b, 1, true, 111));
    m.handle(button_event(a, 1, true, 112));  // どの機器でもよい割り当ては 1 つの入力元にまとまる
    lanes.clear();
    m.collect(120 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(2));
    CHECK(lanes.size() == 2 && lanes[0].lane == 2 && lanes[1].lane == 3);
    // 抜いたら番号が空き、次の機器が使う
    pad_forget(b);
    const uint32_t c2 = 0x7f00a003;
    CHECK_EQ(pad_number(c2), nb);
    pad_forget(a), pad_forget(c2);
}

TEST(input_midi_events) {
    KeyConfig c = KeyConfig::lr2_default();
    InputMapper m(c);
    std::vector<LaneInput> lanes;
    std::vector<ButtonInput> buttons;
    CHECK(InputMapper::midi_event_type() != 0);
    int src = -1;
    bool down = false;
    CHECK(read_midi_event(midi_event(60, true, 5), src, down) && src == 60 && down);
    CHECK(!read_midi_event(key_event(SDL_SCANCODE_Z, true, 0), src, down));
    m.handle(midi_event(41, true, 1000));             // 1 鍵
    m.handle(midi_event(kMidiBendMinus, true, 1001));  // 皿
    m.handle(midi_event(41, false, 1030));
    m.collect(1040 * 1000000ull, lanes, buttons);
    CHECK_EQ(lanes.size(), size_t(3));
    CHECK(lanes.size() == 3 && lanes[0].lane == 1 && lanes[0].down && lanes[0].time_ns == 1000 * 1000000ull);
    CHECK(lanes.size() == 3 && lanes[1].lane == 0 && lanes[1].down);
    CHECK(lanes.size() == 3 && lanes[2].lane == 1 && !lanes[2].down);
    CHECK(m.button_held(kScratchA));
}

TEST(midi_decoder) {
    MidiDecoder d;
    std::vector<std::pair<int, bool>> out;
    d.feed(0x90, 60, 100, out);  // ノートオン
    CHECK(out.size() == 1 && out[0] == std::make_pair(60, true));
    out.clear();
    d.feed(0x93, 60, 90, out);  // ほかのチャンネルでも同じノート。押したままなので何も出さない
    CHECK(out.empty());
    d.feed(0x90, 60, 0, out);  // ベロシティ 0 は離す
    CHECK(out.size() == 1 && out[0] == std::make_pair(60, false));
    out.clear();
    d.feed(0x91, 61, 1, out);
    d.feed(0x81, 61, 64, out);  // ノートオフ
    CHECK(out.size() == 2 && !out[1].second);
    out.clear();
    // ピッチベンド：上位 7 ビットが 64 より下で −、上で +、64 で両方離す
    d.feed(0xe0, 0, 10, out);
    CHECK(out.size() == 1 && out[0] == std::make_pair(int(kMidiBendMinus), true));
    out.clear();
    d.feed(0xe0, 127, 20, out);  // 下位 7 ビットは見ない
    CHECK(out.empty());
    d.feed(0xe0, 0, 100, out);
    CHECK(out.size() == 2 && out[0] == std::make_pair(int(kMidiBendMinus), false) &&
          out[1] == std::make_pair(int(kMidiBendPlus), true));
    out.clear();
    d.feed(0xe0, 0, 64, out);
    CHECK(out.size() == 1 && out[0] == std::make_pair(int(kMidiBendPlus), false));
    out.clear();
    // ペダル（CC 64）：64 より上で押す、下で離す、64 ちょうどは変えない
    d.feed(0xb0, 64, 127, out);
    CHECK(out.size() == 1 && out[0] == std::make_pair(int(kMidiPedal), true));
    out.clear();
    d.feed(0xb0, 64, 64, out);
    d.feed(0xb0, 7, 0, out);  // ほかの CC は使わない
    CHECK(out.empty());
    d.feed(0xb0, 64, 0, out);
    CHECK(out.size() == 1 && !out[0].second);
    out.clear();
    d.feed(0xf8, 0, 0, out);  // クロック
    d.feed(0xa0, 60, 50, out);  // アフタータッチ
    CHECK(out.empty());
    // 機器を抜いたら押しているものを全部離す
    d.feed(0x90, 30, 1, out);
    d.feed(0xe0, 0, 0, out);
    out.clear();
    d.release_all(out);
    CHECK_EQ(out.size(), size_t(2));
}
