#include "input/keyconfig.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

bool parse_binding(std::string_view s, Binding& b) {
    s = trim(s);
    auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };
    if (starts("key:")) {
        std::string name(trim(s.substr(4)));
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        b = {Binding::Type::Key, sc, 0};
        return true;
    }
    if (starts("joy:button ")) {
        b = {Binding::Type::JoyButton, std::atoi(std::string(s.substr(11)).c_str()), 0};
        return true;
    }
    if (starts("joy:axis ")) {
        std::string v(trim(s.substr(9)));
        if (v.empty()) return false;
        int dir = v.back() == '-' ? -1 : 1;
        b = {Binding::Type::JoyAxis, std::atoi(v.c_str()), dir};
        return true;
    }
    if (starts("joy:hat ")) {
        std::istringstream in{std::string(s.substr(8))};
        int idx;
        std::string d;
        in >> idx >> d;
        int dir = d == "up" ? SDL_HAT_UP : d == "down" ? SDL_HAT_DOWN : d == "left" ? SDL_HAT_LEFT : d == "right" ? SDL_HAT_RIGHT : 0;
        if (!dir) return false;
        b = {Binding::Type::JoyHat, idx, dir};
        return true;
    }
    return false;
}

}  // namespace

const char* button_name(int button) { return button >= 0 && button < kButtonCount ? kNames[button] : "?"; }

std::string describe(const Binding& b) {
    char buf[96];
    switch (b.type) {
    case Binding::Type::Key: std::snprintf(buf, sizeof(buf), "key:%s", SDL_GetScancodeName(static_cast<SDL_Scancode>(b.index))); break;
    case Binding::Type::JoyButton: std::snprintf(buf, sizeof(buf), "joy:button %d", b.index); break;
    case Binding::Type::JoyAxis: std::snprintf(buf, sizeof(buf), "joy:axis %d%c", b.index, b.dir < 0 ? '-' : '+'); break;
    case Binding::Type::JoyHat: std::snprintf(buf, sizeof(buf), "joy:hat %d %s", b.index, hat_name(b.dir)); break;
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
