#include "render/keyconfig_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "input/input.h"
#include "input/joystick_init.h"
#include "input/midi.h"
#include "input/win_rawhid.h"
#include "render/frame_hooks.h"

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
    BindingCapture capture;
    capture.wait_center = false;
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
            if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
                std::snprintf(buf, sizeof(buf), "JOY%d BUTTON %d DOWN", pad_number(e.jbutton.which), e.jbutton.button);
            if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION)
                std::snprintf(buf, sizeof(buf), "JOY%d HAT %d = %d", pad_number(e.jhat.which), e.jhat.hat, e.jhat.value);
            int src = 0;
            bool dn = false;
            if (read_midi_event(e, src, dn))
                std::snprintf(buf, sizeof(buf), "%s %s", binding_label({Binding::Type::Midi, src, 0}).c_str(), dn ? "ON" : "OFF");
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
                } else if (Binding b; row < kButtonCount && capture.take(e, b, cfg.analog_axis)) {
                    add_binding(b);
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
                    message = "RESET TO DEFAULT";
                    break;
                default: break;
                }
            }
            mapper->handle(e);
        }

        joystick_init().open_new();
        {
            // 一覧の表示用に、開いているコントローラーを数え直す
            int n = 0;
            SDL_JoystickID* ids = SDL_GetJoysticks(&n);
            for (int i = 0; ids && i < n; ++i)
                if (SDL_Joystick* j = SDL_GetJoystickFromID(ids[i])) joys[ids[i]] = j;
            SDL_free(ids);
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
                if (cfg.analog_axis < 0) std::snprintf(b, sizeof(b), "OFF");
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
        for (auto& [id, j] : joys) {
            const char* name = SDL_GetJoystickName(j);
            std::snprintf(b, sizeof(b), "JOY%d: %s  (%d BUTTONS, %d AXES, %d HATS)", pad_number(id), name ? name : "?",
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
        // Windows：Raw Input で直接読んでいる機器
        for (const RawHidDevice& d : raw_hid().devices()) {
            std::snprintf(b, sizeof(b), "JOY%d (RAW): %s  (%d BUTTONS, %d AXES, %d HATS)", pad_number(d.id), d.name.c_str(), d.buttons,
                          d.axes, d.hats);
            text(r, 16, y, dim, b);
            y += 12;
            for (int a = 0; a < std::min<int>(8, static_cast<int>(d.axis_values.size())); ++a) {
                int16_t v = d.axis_values[a];
                float bx = 16 + (a % 4) * 156.0f, by = y + (a / 4) * 14.0f;
                fill(r, bx + 40, by, 100, 8, {40, 40, 50, 255});
                fill(r, bx + 40 + (v + 32768) / 65536.0f * 96, by, 4, 8, a == cfg.analog_axis ? hi : on);
                std::snprintf(b, sizeof(b), "A%d", a);
                text(r, bx, by, dim, b);
            }
            y += ((std::min<int>(8, static_cast<int>(d.axis_values.size())) + 3) / 4) * 14.0f + 4;
        }
        for (const std::string& name : midi_input().devices()) text(r, 16, y, dim, "MIDI: " + name), y += 12;
        if (!joystick_init().ready()) text(r, 16, y, hi, "PREPARING CONTROLLERS..."), y += 12;
        else if (joys.empty() && raw_hid().devices().empty()) text(r, 16, y, dim, "NO JOYSTICK CONNECTED"), y += 12;
        text(r, 16, y + 4, dim, "LAST: " + last_raw);
        text(r, 16, 440, hi, message);
        text(r, 16, 456, dim, "UP/DOWN: SELECT   F1: RESET TO DEFAULT");
        if (const char* shot = std::getenv("BMJ_KEYCONFIG_SHOT"); shot && SDL_GetTicksNS() - t0 > 1500000000ull) {
            // 開発用：1枚保存して終わる
            if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            quit = true;
        }
        frame_hooks_before_present(r);
        SDL_RenderPresent(r);
    }
    for (auto& [id, j] : joys) SDL_CloseJoystick(j);
    if (cfg.save(path)) std::printf("キーコンフィグを保存した: %s\n", path.c_str());
    else std::printf("キーコンフィグを保存できない: %s\n", path.c_str());
    (void)window;
}

}  // namespace bmj

#include "render/text_cache.h"

namespace bmj {

namespace {

bool is_key(const Binding& b) { return b.type == Binding::Type::Key; }
// 入力の種類（キーボード／パッド／MIDI）
int kind(const Binding& b) { return is_key(b) ? 0 : b.type == Binding::Type::Midi ? 2 : 1; }

// 割り当てを入れ替える：同じ種類（キーボード／パッド／MIDI）の割り当てを消してから足し、ほかのボタンからは外す
void assign(KeyConfig& cfg, int button, const Binding& b) {
    for (int i = 0; i < kButtonCount; ++i) {
        auto& v = cfg.buttons[i];
        v.erase(std::remove(v.begin(), v.end(), b), v.end());
    }
    auto& v = cfg.buttons[button];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Binding& x) { return kind(x) == kind(b); }), v.end());
    v.push_back(b);
}

}  // namespace

bool run_key_wizard(SDL_Renderer* r, TextCache& text, KeyConfig& cfg_in, const std::string& path) {
    KeyConfig cfg = cfg_in;
    // 手順：0..6 = 鍵盤1〜7、7 = 皿の右回し（時計回り）、8 = 皿の左回し（反時計回り）、9 = 確認。
    // 右回し = scratch_b（LR2 の key11。皿の絵が時計回りに回る）、左回し = scratch_a（key10）。
    // アナログの皿も両方の向きを回してもらい、2回の向きが逆になっていることを確かめる
    int step = 0;
    bool scratch_by_button = false;
    uint32_t analog_which = 0;
    int cw_sign = 0;  // 右回ししたときに軸の値が動いた向き
    std::string warn;
    Binding cw_binding{};
    // 軸の動きは機器ごとに数える（同じ番号の軸を持つ機器が2台あると、混ぜたときに大きく動いたように見える）
    struct AxisTrack {
        int64_t travel = 0, net = 0;
        int16_t prev = 0;
        int small_steps = 0;  // 少しずつ動いた回数（位置を返す皿なら増える）
        int pulses = 0, pulse_dir = 0;  // 中央から端へ跳んだ回数と向き（向きだけを送る皿）
    };
    std::map<std::pair<uint32_t, int>, AxisTrack> axes;
    std::string last;
    uint64_t step_start = SDL_GetTicksNS();
    auto next_step = [&](int s) {
        step = s;
        warn.clear();
        axes.clear();
        step_start = SDL_GetTicksNS();
    };
    auto bind_digital = [&](const Binding& b) {
        last = describe(b);
        if (step <= 6) {
            assign(cfg, kKey1 + step, b);
            next_step(step + 1);
        } else if (step == 7) {
            // 皿をボタンやキーで操作する：右回し。アナログの皿の設定は切る
            assign(cfg, kScratchB, b);
            cw_binding = b;
            if (!is_key(b)) cfg.analog_axis = -1;
            scratch_by_button = true;
            next_step(8);
        } else if (step == 8 && scratch_by_button) {
            if (b == cw_binding) {
                warn = "右回しと同じ入力です。反時計回りに回してください";
                return;
            }
            assign(cfg, kScratchA, b);
            next_step(9);
        }
    };

    BindingCapture capture;  // パッドのボタン・ハットと MIDI（軸はこの画面で別に見る）
    int midi_src = 0;
    bool midi_down = false;
    bool done = false, canceled = false;
    while (!done && !canceled) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) canceled = true;
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) SDL_OpenJoystick(e.jdevice.which);
            // 手順が変わった直後の入力（前の手順で押したものの離し・揺れ）は少しの間無視する
            bool settle = SDL_GetTicksNS() - step_start < 150'000'000ull;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    canceled = true;
                } else if (e.key.scancode == SDL_SCANCODE_BACKSPACE) {
                    next_step(std::max(0, step - 1));
                } else if (e.key.scancode == SDL_SCANCODE_RETURN && step == 8) {
                    // 左回しは無しでよい（アナログなら右回しの向きだけで決める）
                    if (!scratch_by_button) cfg.analog_invert = cw_sign < 0;
                    next_step(9);
                } else if (e.key.scancode == SDL_SCANCODE_RETURN && step == 9) {
                    done = true;
                } else if (!settle && step <= 8) {
                    bind_digital({Binding::Type::Key, e.key.scancode, 0});
                }
            } else if ((e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || read_midi_event(e, midi_src, midi_down)) && !settle) {
                // パッドのボタンと MIDI。確認の手順なら決定
                Binding b;
                if (!capture.take(e, b)) continue;
                if (step == 9) done = true;
                else bind_digital(b);
            } else if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && e.jhat.value && !settle && step <= 8) {
                if (Binding b; capture.take(e, b)) bind_digital(b);
            } else if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && e.jaxis.axis < 16) {
                // 軸は皿の手順でだけ見る（専用コントローラーの皿はアナログの軸として来ることが多く、鍵盤の手順で拾うと誤る）
                int a = e.jaxis.axis;
                auto [it, first] = axes.try_emplace({e.jaxis.which, a});
                AxisTrack& t = it->second;
                if (!first) {
                    int d = static_cast<int>(e.jaxis.value) - t.prev;
                    // 1周で値が端から端へ戻るときの折り返し
                    if (d > 32768) d -= 65536;
                    if (d < -32768) d += 65536;
                    t.travel += std::abs(d);
                    t.net += d;
                    if (d != 0 && std::abs(d) < 8000) t.small_steps++;
                }
                // 中央から一気に端へ跳んだ軸は、回している向きだけを送る皿（止まると中央に戻る）。
                // LR2 と同じく、軸を方向ボタンとして割り当てる
                // 2回続けて同じ向きに跳んだら決める（1回だと、位置を返す皿の最初の値と区別できない）
                if (std::abs(e.jaxis.value) > 24000 && (first || std::abs(t.prev) < 4000)) {
                    int dir = e.jaxis.value > 0 ? 1 : -1;
                    t.pulses = dir == t.pulse_dir ? t.pulses + 1 : 1;
                    t.pulse_dir = dir;
                }
                t.prev = e.jaxis.value;
                if ((step == 7 || (step == 8 && scratch_by_button)) && !settle && t.pulses >= 2 && t.small_steps == 0) {
                    bind_digital({Binding::Type::JoyAxis, a, e.jaxis.value > 0 ? 1 : -1, capture_device(e.jaxis.which)});
                    continue;
                }
                // 位置を返す皿（少しずつ動く）だけをアナログとして扱う
                bool positional = t.small_steps >= 4 && t.travel > 12000;
                if (step == 7 && !settle && positional) {
                    cfg.analog_axis = a;
                    analog_which = e.jaxis.which;
                    cw_sign = t.net < 0 ? -1 : 1;
                    scratch_by_button = false;
                    // アナログの皿にするので、軸を方向ボタンとして読む古い割り当ては外す
                    for (int b : {kScratchA, kScratchB}) {
                        auto& v = cfg.buttons[b];
                        v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& x) { return x.type == Binding::Type::JoyAxis; }), v.end());
                    }
                    char m[48];
                    std::snprintf(m, sizeof(m), "joy:axis %d（アナログ）右回し", a);
                    last = m;
                    next_step(8);
                } else if (step == 8 && !scratch_by_button && !settle && e.jaxis.which == analog_which &&
                           a == cfg.analog_axis && positional) {
                    if ((t.net < 0 ? -1 : 1) == cw_sign) {
                        // 右回しと同じ向きに回っている。数え直す
                        warn = "右回しと同じ向きに回っています。反時計回りに回してください";
                        axes.clear();
                    } else {
                        // 右に回したときに値が減る皿なら向きを反転する（右回し = scratch_b になるように）
                        cfg.analog_invert = cw_sign < 0;
                        char m[48];
                        std::snprintf(m, sizeof(m), "joy:axis %d（アナログ%s）左回し", a, cfg.analog_invert ? "・反転" : "");
                        last = m;
                        next_step(9);
                    }
                }
            }
        }

        // 描画
        SDL_SetRenderDrawColor(r, 10, 10, 18, 255);
        SDL_RenderClear(r);
        SDL_Color white{235, 235, 240, 255}, dim{140, 140, 155, 255}, accent{255, 210, 90, 255}, on{80, 220, 140, 255};
        text.draw(640, 60, 26, white, "キーコンフィグ", 1);
        std::string msg;
        if (step <= 6) msg = std::to_string(step + 1) + " 鍵を押してください";
        else if (step == 7) msg = "皿を右（時計回り）に回してください（ボタンやキーで操作するなら、右回しに使うものを押す）";
        else if (step == 8 && scratch_by_button) msg = "皿の左（反時計回り）に使うものを押してください（無ければ Enter）";
        else if (step == 8) msg = "皿を左（反時計回り）に回してください";
        else msg = "設定できました。決定（Enter か コントローラーのボタン）で保存します";
        text.draw(640, 170, 30, accent, msg, 1, 1180);
        // 鍵盤の図：皿＋7鍵。いま聞いているところを光らせる
        float x0 = 640 - (546 + 20) / 2.0f + 20;  // 皿の左端から7鍵の右端までを中央に
        for (int lane = 0; lane < 8; ++lane) {
            float x = x0 + lane * 70 + (lane == 0 ? -20 : 0);
            float w = lane == 0 ? 80 : 56;
            bool now = (lane == 0 && (step == 7 || step == 8)) || (lane >= 1 && step == lane - 1);
            bool set = lane == 0 ? step >= 9 : step > lane - 1;
            SDL_FRect rc{x, 280, w, lane == 0 ? 120.0f : (lane % 2 ? 140.0f : 100.0f)};
            SDL_Color c = now ? accent : set ? on : SDL_Color{50, 50, 64, 255};
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
            SDL_RenderFillRect(r, &rc);
            text.draw(x + w / 2, 430, 18, now ? accent : dim, lane == 0 ? "皿" : std::to_string(lane), 1);
        }
        if (!last.empty()) text.draw(640, 490, 18, dim, "直前に割り当てた入力：" + last, 1);
        if (!warn.empty()) text.draw(640, 230, 20, SDL_Color{255, 110, 110, 255}, warn, 1, 1180);
        text.draw(640, 640, 16, dim, "Backspace：1つ戻る　　Esc：やめる（保存しない）", 1);
        if (const char* shot = std::getenv("BMJ_WIZARD_SHOT")) {
            // 開発用：1枚保存してやめる
            if (SDL_Surface* sf = SDL_RenderReadPixels(r, nullptr)) SDL_SaveBMP(sf, shot), SDL_DestroySurface(sf);
            canceled = true;
        }
        frame_hooks_before_present(r);
        SDL_RenderPresent(r);
        SDL_DelayPrecise(4'000'000);
    }
    if (canceled) return false;
    cfg_in = cfg;
    if (cfg.save(path)) std::printf("キーコンフィグを保存した: %s\n", path.c_str());
    return true;
}

}  // namespace bmj
