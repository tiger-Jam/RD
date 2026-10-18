#include "render/keyconfig_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "input/input.h"

namespace bmj {

namespace {

void text(SDL_Renderer* r, float x, float y, SDL_Color c, const std::string& s, float scale = 1.0f) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, s.c_str());
    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

void fill(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

const char* const kLabels[kButtonCount] = {"KEY 1", "KEY 2", "KEY 3", "KEY 4", "KEY 5", "KEY 6", "KEY 7",
                                           "SCRATCH A", "SCRATCH B", "START", "SELECT"};
constexpr int kRowAnalog = kButtonCount;      // アナログ皿の軸
constexpr int kRowRelease = kButtonCount + 1; // アナログ皿が止まったとみなす時間
constexpr int kRows = kButtonCount + 2;

}  // namespace

void run_keyconfig(SDL_Window* window, SDL_Renderer* r, KeyConfig& cfg, const std::string& path) {
    SDL_SetRenderLogicalPresentation(r, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    int row = 0;
    bool capturing = false;
    std::string message = "ENTER: ADD  BACKSPACE: CLEAR ROW  ESC: SAVE & QUIT";
    auto mapper = std::make_unique<InputMapper>(cfg);
    std::map<SDL_JoystickID, SDL_Joystick*> joys;
    // 割り当ての取り込み中に、軸がどれだけ動いたか（アナログ皿の軸を見つけるため）
    std::array<int64_t, 16> axis_travel{};
    std::array<int16_t, 16> axis_prev{};
    std::array<bool, 16> axis_seen{};
    std::array<int16_t, 16> axis_now{};
    std::string last_raw;
    // 皿の確認用：押下の回数と、最後に押下・離した時刻
    int scratch_presses = 0;
    double scratch_down_ms = -1, scratch_up_ms = -1;
    const uint64_t t0 = SDL_GetTicksNS();

    auto rebuild = [&] { mapper = std::make_unique<InputMapper>(cfg); };
    auto add_binding = [&](const Binding& b) {
        auto& v = cfg.buttons[row];
        if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
        capturing = false;
        message = std::string("ADDED ") + describe(b) + " TO " + kLabels[row];
        rebuild();
    };

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                if (SDL_Joystick* j = SDL_OpenJoystick(e.jdevice.which)) joys[e.jdevice.which] = j;
            }
            if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
                if (auto it = joys.find(e.jdevice.which); it != joys.end()) SDL_CloseJoystick(it->second), joys.erase(it);
            }
            // 生の入力の表示
            char buf[96] = "";
            if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) std::snprintf(buf, sizeof(buf), "JOY BUTTON %d DOWN", e.jbutton.button);
            if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION) std::snprintf(buf, sizeof(buf), "JOY HAT %d = %d", e.jhat.hat, e.jhat.value);
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) std::snprintf(buf, sizeof(buf), "KEY %s", SDL_GetScancodeName(e.key.scancode));
            if (buf[0]) last_raw = buf;
            if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && e.jaxis.axis < 16) {
                int a = e.jaxis.axis;
                if (axis_seen[a]) {
                    int d = std::abs(static_cast<int>(e.jaxis.value) - axis_prev[a]);
                    if (d > 32768) d = 65536 - d;
                    axis_travel[a] += d;
                }
                axis_seen[a] = true;
                axis_prev[a] = e.jaxis.value;
                axis_now[a] = e.jaxis.value;
            }

            if (capturing) {
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                        capturing = false;
                        message = "CANCELED";
                    } else if (row < kButtonCount) {
                        add_binding({Binding::Type::Key, e.key.scancode, 0});
                    }
                } else if (row < kButtonCount && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
                    add_binding({Binding::Type::JoyButton, e.jbutton.button, 0});
                } else if (row < kButtonCount && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && e.jhat.value) {
                    int v = e.jhat.value;
                    int dir = v & SDL_HAT_UP ? SDL_HAT_UP : v & SDL_HAT_DOWN ? SDL_HAT_DOWN : v & SDL_HAT_LEFT ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                    add_binding({Binding::Type::JoyHat, e.jhat.hat, dir});
                } else if (row < kButtonCount && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION &&
                           std::abs(e.jaxis.value) > 24000 && e.jaxis.axis != cfg.analog_axis) {
                    add_binding({Binding::Type::JoyAxis, e.jaxis.axis, e.jaxis.value > 0 ? 1 : -1});
                } else if (row == kRowAnalog && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
                    // 一番大きく動いた軸をアナログ皿にする（1周の1/4くらい回したら決める）
                    int best = -1;
                    for (int a = 0; a < 16; ++a)
                        if (axis_travel[a] > 16384 && (best < 0 || axis_travel[a] > axis_travel[best])) best = a;
                    if (best >= 0) {
                        cfg.analog_axis = best;
                        capturing = false;
                        char m[64];
                        std::snprintf(m, sizeof(m), "ANALOG SCRATCH = AXIS %d", best);
                        message = m;
                        rebuild();
                    }
                }
                continue;
            }

            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                switch (e.key.scancode) {
                case SDL_SCANCODE_ESCAPE: quit = true; break;
                case SDL_SCANCODE_UP: row = (row + kRows - 1) % kRows; break;
                case SDL_SCANCODE_DOWN: row = (row + 1) % kRows; break;
                case SDL_SCANCODE_RETURN:
                    if (row == kRowRelease) break;
                    capturing = true;
                    axis_travel.fill(0);
                    message = row == kRowAnalog ? "TURN THE TURNTABLE...  (ESC: CANCEL)" : "PRESS A KEY OR BUTTON...  (ESC: CANCEL)";
                    break;
                case SDL_SCANCODE_BACKSPACE:
                case SDL_SCANCODE_DELETE:
                    if (row < kButtonCount) cfg.buttons[row].clear();
                    else if (row == kRowAnalog) cfg.analog_axis = -1;
                    rebuild();
                    message = "CLEARED";
                    break;
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_RIGHT: {
                    int d = e.key.scancode == SDL_SCANCODE_LEFT ? -1 : 1;
                    if (row == kRowRelease) cfg.analog_release_ms = std::clamp(cfg.analog_release_ms + d * 10, 10, 500);
                    if (row == kRowAnalog) cfg.analog_invert = !cfg.analog_invert;
                    rebuild();
                    break;
                }
                case SDL_SCANCODE_F1:
                    cfg = KeyConfig::lr2_default();
                    rebuild();
                    message = "RESET TO LR2 DEFAULT";
                    break;
                default: break;
                }
            }
            mapper->handle(e);
        }

        uint64_t now = SDL_GetTicksNS();
        std::vector<LaneInput> lanes;
        std::vector<ButtonInput> buttons;
        mapper->collect(now, lanes, buttons);
        for (const LaneInput& li : lanes) {
            if (li.lane != 0) continue;
            double ms = (li.time_ns - t0) / 1e6;
            if (li.down) scratch_presses++, scratch_down_ms = ms;
            else scratch_up_ms = ms;
        }

        // 描画
        SDL_SetRenderDrawColor(r, 8, 8, 16, 255);
        SDL_RenderClear(r);
        SDL_Color white{230, 230, 230, 255}, dim{140, 140, 150, 255}, hi{255, 220, 80, 255}, on{80, 255, 140, 255};
        text(r, 16, 12, white, "KEY CONFIG", 2.0f);
        for (int i = 0; i < kRows; ++i) {
            float y = 44 + i * 18.0f;
            bool sel = i == row;
            if (sel) fill(r, 10, y - 4, 620, 16, capturing ? SDL_Color{90, 60, 10, 255} : SDL_Color{40, 40, 70, 255});
            std::string label, value;
            bool lit = false;
            if (i < kButtonCount) {
                label = kLabels[i];
                for (size_t k = 0; k < cfg.buttons[i].size(); ++k) value += (k ? " | " : "") + describe(cfg.buttons[i][k]);
                if (value.empty()) value = "(NONE)";
                lit = mapper->button_held(i);
            } else if (i == kRowAnalog) {
                label = "ANALOG TT";
                char b[64];
                if (cfg.analog_axis < 0) std::snprintf(b, sizeof(b), "OFF (LR2 STYLE)");
                else std::snprintf(b, sizeof(b), "AXIS %d%s   (LEFT/RIGHT: INVERT)", cfg.analog_axis, cfg.analog_invert ? " INVERTED" : "");
                value = b;
            } else {
                label = "TT RELEASE";
                value = std::to_string(cfg.analog_release_ms) + " ms   (LEFT/RIGHT)";
            }
            if (lit) fill(r, 14, y - 2, 6, 12, on);
            text(r, 26, y, sel ? hi : white, label);
            text(r, 120, y, sel ? hi : dim, value.size() > 62 ? value.substr(0, 62) : value);
        }

        // 入力の確認
        float y = 44 + kRows * 18.0f + 10;
        text(r, 16, y, white, "INPUT TEST");
        y += 16;
        for (int lane = 0; lane < 8; ++lane) {
            bool held = lane == 0 ? (mapper->button_held(kScratchA) || mapper->button_held(kScratchB))
                                  : mapper->button_held(kKey1 + lane - 1);
            SDL_Color c = held ? on : SDL_Color{40, 40, 50, 255};
            fill(r, 16 + lane * 34.0f, y, 28, 20, c);
            text(r, 24 + lane * 34.0f, y + 6, held ? SDL_Color{0, 0, 0, 255} : dim, lane == 0 ? "S" : std::to_string(lane));
        }
        char b[128];
        std::snprintf(b, sizeof(b), "SCRATCH PRESSES %d  LAST DOWN %.0f  UP %.0f", scratch_presses, scratch_down_ms, scratch_up_ms);
        text(r, 300, y + 6, dim, b);
        y += 30;
        int ji = 0;
        for (auto& [id, j] : joys) {
            const char* name = SDL_GetJoystickName(j);
            std::snprintf(b, sizeof(b), "JOY %d: %s  (%d BUTTONS, %d AXES, %d HATS)", ji++, name ? name : "?",
                          SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickHats(j));
            text(r, 16, y, dim, b);
            y += 12;
            int axes = std::min(8, SDL_GetNumJoystickAxes(j));
            for (int a = 0; a < axes; ++a) {
                int16_t v = SDL_GetJoystickAxis(j, a);
                float bx = 16 + (a % 4) * 156.0f, by = y + (a / 4) * 14.0f;
                fill(r, bx + 40, by, 100, 8, {40, 40, 50, 255});
                fill(r, bx + 40 + (v + 32768) / 65536.0f * 96, by, 4, 8, a == cfg.analog_axis ? hi : on);
                std::snprintf(b, sizeof(b), "A%d", a);
                text(r, bx, by, dim, b);
            }
            y += ((axes + 3) / 4) * 14.0f + 4;
        }
        if (joys.empty()) text(r, 16, y, dim, "NO JOYSTICK CONNECTED"), y += 12;
        text(r, 16, y + 4, dim, "LAST: " + last_raw);
        text(r, 16, 440, hi, message);
        text(r, 16, 456, dim, "UP/DOWN: SELECT   F1: RESET TO LR2 DEFAULT");
        if (const char* shot = std::getenv("BMJ_KEYCONFIG_SHOT")) {
            // 開発用：1枚保存して終わる
            if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            quit = true;
        }
        SDL_RenderPresent(r);
    }
    for (auto& [id, j] : joys) SDL_CloseJoystick(j);
    if (cfg.save(path)) std::printf("キーコンフィグを保存した: %s\n", path.c_str());
    else std::printf("キーコンフィグを保存できない: %s\n", path.c_str());
    (void)window;
}

}  // namespace bmj
