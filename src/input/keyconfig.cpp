#include "input/keyconfig.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>

#include "util/text.h"

namespace bmj {

namespace {

const char* const kNames[kButtonCount] = {"key1", "key2", "key3", "key4", "key5", "key6", "key7",
                                          "scratch_a", "scratch_b", "start", "select"};

const char* hat_name(int dir) {
    switch (dir) {
    case SDL_HAT_UP: return "up";
    case SDL_HAT_DOWN: return "down";
    case SDL_HAT_LEFT: return "left";
    case SDL_HAT_RIGHT: return "right";
    default: return "?";
    }
}

// 「joy2:」の機器の番号を読む。番号が無ければ 0（どのパッドでもよい）
bool split_joy(std::string_view s, int& device, std::string_view& rest) {
    if (s.rfind("joy", 0) != 0) return false;
    size_t colon = s.find(':');
    if (colon == std::string_view::npos) return false;
    std::string_view num = s.substr(3, colon - 3);
    device = 0;
    for (char c : num) {
        if (c < '0' || c > '9') return false;
        device = device * 10 + (c - '0');
        if (device > 99) return false;
    }
    if (!num.empty() && device == 0) return false;  // joy0: は使わない
    rest = s.substr(colon + 1);
    return true;
}

}  // namespace

bool parse_binding(std::string_view s, Binding& b) {
    s = trim(s);
    auto starts = [&](std::string_view t, const char* p) { return t.rfind(p, 0) == 0; };
    if (starts(s, "key:")) {
        std::string name(trim(s.substr(4)));
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        b = {Binding::Type::Key, sc, 0};
        return true;
    }
    if (starts(s, "midi:")) {
        std::string_view r = trim(s.substr(5));
        if (starts(r, "note ")) {
            int n = std::atoi(std::string(trim(r.substr(5))).c_str());
            if (n < 0 || n > 127) return false;
            b = {Binding::Type::Midi, n, 0};
            return true;
        }
        if (r == "bend -" || r == "bend +") {
            b = {Binding::Type::Midi, r.back() == '-' ? kMidiBendMinus : kMidiBendPlus, 0};
            return true;
        }
        if (r == "pedal") {
            b = {Binding::Type::Midi, kMidiPedal, 0};
            return true;
        }
        return false;
    }
    int device = 0;
    std::string_view r;
    if (!split_joy(s, device, r)) return false;
    if (starts(r, "button ")) {
        b = {Binding::Type::JoyButton, std::atoi(std::string(r.substr(7)).c_str()), 0, device};
        return true;
    }
    if (starts(r, "axis ")) {
        std::string v(trim(r.substr(5)));
        if (v.empty()) return false;
        int dir = v.back() == '-' ? -1 : 1;
        b = {Binding::Type::JoyAxis, std::atoi(v.c_str()), dir, device};
        return true;
    }
    if (starts(r, "hat ")) {
        std::istringstream in{std::string(r.substr(4))};
        int idx;
        std::string d;
        in >> idx >> d;
        int dir = d == "up" ? SDL_HAT_UP : d == "down" ? SDL_HAT_DOWN : d == "left" ? SDL_HAT_LEFT : d == "right" ? SDL_HAT_RIGHT : 0;
        if (!dir) return false;
        b = {Binding::Type::JoyHat, idx, dir, device};
        return true;
    }
    return false;
}

const char* button_name(int button) { return button >= 0 && button < kButtonCount ? kNames[button] : "?"; }

std::string describe(const Binding& b) {
    char buf[96];
    // 機器を決めたパッドは joy2: のように番号を付ける（番号の無い joy: はどのパッドでもよい）
    char joy[16] = "joy";
    if (b.device > 0) std::snprintf(joy, sizeof(joy), "joy%d", b.device);
    switch (b.type) {
    case Binding::Type::Key: std::snprintf(buf, sizeof(buf), "key:%s", SDL_GetScancodeName(static_cast<SDL_Scancode>(b.index))); break;
    case Binding::Type::JoyButton: std::snprintf(buf, sizeof(buf), "%s:button %d", joy, b.index); break;
    case Binding::Type::JoyAxis: std::snprintf(buf, sizeof(buf), "%s:axis %d%c", joy, b.index, b.dir < 0 ? '-' : '+'); break;
    case Binding::Type::JoyHat: std::snprintf(buf, sizeof(buf), "%s:hat %d %s", joy, b.index, hat_name(b.dir)); break;
    case Binding::Type::Midi:
        if (b.index == kMidiBendMinus) std::snprintf(buf, sizeof(buf), "midi:bend -");
        else if (b.index == kMidiBendPlus) std::snprintf(buf, sizeof(buf), "midi:bend +");
        else if (b.index == kMidiPedal) std::snprintf(buf, sizeof(buf), "midi:pedal");
        else std::snprintf(buf, sizeof(buf), "midi:note %d", b.index);
        break;
    }
    return buf;
}

void set_slot(KeyConfig& cfg, int button, int slot, const Binding& b) {
    if (button < 0 || button >= kButtonCount || slot < 0 || slot >= kKeySlots) return;
    auto& v = cfg.buttons[button];
    auto it = std::find(v.begin(), v.end(), b);
    if (it != v.end()) {
        int j = static_cast<int>(it - v.begin());
        if (j == slot) return;
        v.erase(it);
        if (j < slot) --slot;  // 前のスロットが詰まった分
    }
    if (slot < static_cast<int>(v.size())) v[slot] = b;
    else v.push_back(b);
}

void clear_slot(KeyConfig& cfg, int button, int slot) {
    if (button < 0 || button >= kButtonCount || slot < 0) return;
    auto& v = cfg.buttons[button];
    if (slot < static_cast<int>(v.size())) v.erase(v.begin() + slot);
}

void clear_bindings(KeyConfig& cfg) {
    for (auto& v : cfg.buttons) v.clear();
}

std::string binding_label(const Binding& b) {
    char buf[64];
    char joy[16] = "JOY";
    if (b.device > 0) std::snprintf(joy, sizeof(joy), "JOY%d", b.device);
    switch (b.type) {
    case Binding::Type::Key: {
        std::string s = SDL_GetScancodeName(static_cast<SDL_Scancode>(b.index));
        if (s.empty()) s = "KEY" + std::to_string(b.index);
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    case Binding::Type::JoyButton: std::snprintf(buf, sizeof(buf), "%s:BUTTON%d", joy, b.index + 1); break;
    case Binding::Type::JoyAxis:
        // 軸 0 は左右、軸 1 は上下（LR2 の方向の名前に合わせる）
        if (b.index == 0) std::snprintf(buf, sizeof(buf), "%s:%s", joy, b.dir < 0 ? "LEFT" : "RIGHT");
        else if (b.index == 1) std::snprintf(buf, sizeof(buf), "%s:%s", joy, b.dir < 0 ? "UP" : "DOWN");
        else std::snprintf(buf, sizeof(buf), "%s:AXIS%d%c", joy, b.index + 1, b.dir < 0 ? '-' : '+');
        break;
    case Binding::Type::JoyHat: {
        std::string d = hat_name(b.dir);
        for (char& c : d) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::snprintf(buf, sizeof(buf), "%s:HAT%d %s", joy, b.index + 1, d.c_str());
        break;
    }
    case Binding::Type::Midi: {
        // ノートは音名とオクターブ（ノート番号 / 12。60 は C5）
        static const char* const kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        if (b.index == kMidiBendMinus) return "MIDI BEND:-";
        if (b.index == kMidiBendPlus) return "MIDI BEND:+";
        if (b.index == kMidiPedal) return "MIDI PEDAL";
        if (b.index < 0 || b.index > 127) return "MIDI ?";
        std::snprintf(buf, sizeof(buf), "MIDI NOTE:%s%d", kNotes[b.index % 12], b.index / 12);
        break;
    }
    }
    return buf;
}

KeyConfig KeyConfig::lr2_default() {
    KeyConfig c;
    auto key = [&](int button, SDL_Scancode sc) { c.buttons[button].push_back({Binding::Type::Key, sc, 0}); };
    auto pad = [&](int button, int lr2_button) {  // LR2の BUTTON(n) はSDLのボタン n-1
        c.buttons[button].push_back({Binding::Type::JoyButton, lr2_button - 1, 0});
    };
    // LR2の既定（1P）：Z S X D C F V、皿は左Ctrl・スペース／左Shift、START=Q、SELECT=W
    const SDL_Scancode keys[7] = {SDL_SCANCODE_Z, SDL_SCANCODE_S, SDL_SCANCODE_X, SDL_SCANCODE_D,
                                  SDL_SCANCODE_C, SDL_SCANCODE_F, SDL_SCANCODE_V};
    for (int i = 0; i < 7; ++i) key(kKey1 + i, keys[i]);
    key(kScratchA, SDL_SCANCODE_LCTRL);
    key(kScratchA, SDL_SCANCODE_SPACE);
    key(kScratchB, SDL_SCANCODE_LSHIFT);
    key(kStart, SDL_SCANCODE_Q);
    key(kSelect, SDL_SCANCODE_W);
    // ジョイスティック：鍵盤は B4 B7 B3 B8 B2 B5 と左、皿は下／上、START=B9、SELECT=B10
    const int pad_keys[6] = {4, 7, 3, 8, 2, 5};
    for (int i = 0; i < 6; ++i) pad(kKey1 + i, pad_keys[i]);
    c.buttons[kKey7].push_back({Binding::Type::JoyAxis, 0, -1});
    c.buttons[kScratchA].push_back({Binding::Type::JoyAxis, 1, 1});
    c.buttons[kScratchB].push_back({Binding::Type::JoyAxis, 1, -1});
    pad(kStart, 9);
    pad(kSelect, 10);
    // MIDI 鍵盤：鍵盤は 41〜47 と 53〜59、皿は 36・38・48・50 とピッチベンドの −（scratch_a）、
    // 40・52 とベンドの +（scratch_b）、START = 39・51、SELECT = 37・49
    auto midi = [&](int button, std::initializer_list<int> notes) {
        for (int n : notes) c.buttons[button].push_back({Binding::Type::Midi, n, 0});
    };
    for (int i = 0; i < 7; ++i) midi(kKey1 + i, {41 + i, 53 + i});
    midi(kScratchA, {36, 38, 48, 50, kMidiBendMinus});
    midi(kScratchB, {40, 52, kMidiBendPlus});
    midi(kStart, {39, 51});
    midi(kSelect, {37, 49});
    return c;
}

bool KeyConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    KeyConfig c;
    std::string line;
    while (std::getline(f, line)) {
        std::string_view l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(trim(l.substr(0, eq)));
        std::string_view val = trim(l.substr(eq + 1));
        if (key == "analog_axis") c.analog_axis = std::atoi(std::string(val).c_str());
        else if (key == "analog_invert") c.analog_invert = val == "1" || val == "true";
        else if (key == "analog_release_ms") c.analog_release_ms = std::atoi(std::string(val).c_str());
        for (int b = 0; b < kButtonCount; ++b) {
            if (key != kNames[b]) continue;
            // 割り当ては '|' 区切り（キーの名前にカンマや空白が入ることがあるため）
            size_t pos = 0;
            while (pos <= val.size()) {
                size_t bar = val.find('|', pos);
                std::string_view part = val.substr(pos, bar == std::string_view::npos ? std::string_view::npos : bar - pos);
                Binding bind;
                if (parse_binding(part, bind)) c.buttons[b].push_back(bind);
                if (bar == std::string_view::npos) break;
                pos = bar + 1;
            }
        }
    }
    *this = c;
    return true;
}

bool KeyConfig::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "# BMjoy キーコンフィグ（bmjoy --keyconfig で編集できる）\n";
    f << "# 書式: ボタン = 割り当て | 割り当て ...\n";
    f << "#   key:キー名 / joy:button 番号 / joy:axis 番号+ または 番号- / joy:hat 番号 up|down|left|right\n";
    f << "#   joy2:button 番号 のように joy の後に機器の番号（1 から。つないだ順）を付けると、そのパッドだけに効く\n";
    f << "#   midi:note ノート番号 / midi:bend - / midi:bend + / midi:pedal（MIDI はどの機器・チャンネルでも同じ）\n";
    for (int b = 0; b < kButtonCount; ++b) {
        f << kNames[b] << " =";
        for (size_t i = 0; i < buttons[b].size(); ++i) f << (i ? " | " : " ") << describe(buttons[b][i]);
        f << "\n";
    }
    f << "# アナログ皿（-1で使わない）。回している間は押下、止まって analog_release_ms 経ったら離す\n";
    f << "analog_axis = " << analog_axis << "\n";
    f << "analog_invert = " << (analog_invert ? 1 : 0) << "\n";
    f << "analog_release_ms = " << analog_release_ms << "\n";
    return static_cast<bool>(f);
}

std::string keyconfig_path() {
    char* dir = SDL_GetPrefPath(nullptr, "BMjoy");
    std::string p = dir ? std::string(dir) + "keyconfig.txt" : "keyconfig.txt";
    SDL_free(dir);
    return p;
}

}  // namespace bmj
