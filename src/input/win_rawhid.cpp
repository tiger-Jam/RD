#include "input/win_rawhid.h"

#include "input/input.h"
#include "util/dev.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace rd {

namespace {

// SDL のジョイスティックの番号と重ならないように、大きな番号から振る
constexpr uint32_t kIdBase = 0x7f000000;

struct ValueCap {
    USAGE usage;
    LONG lmin, lmax;
    int index;
    int bits = 0;  // 軸ならSDLの軸番号、ハットなら -1
};

struct Device {
    uint32_t id = 0;
    std::string name;
    std::vector<uint8_t> preparsed;
    std::vector<ValueCap> values;
    int buttons = 0, axes = 0, hats = 0;
    std::vector<bool> button_state;
    std::vector<int16_t> axis_state;
    uint8_t hat_state = SDL_HAT_CENTERED;
};

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void push_event(SDL_Event& e, uint64_t ts) {
    // timestamp を入力が届いた瞬間にしておく（判定はこの時刻で行う）
    e.common.timestamp = ts;
    bool ok = SDL_PushEvent(&e);
    static int logged = 0;
    if (dev_env("RD_DEBUG_INPUT") && logged < 20)
        std::printf("RawHid: イベント %u を送った（%s）%s\n", e.type, ok ? "成功" : "失敗", ok ? "" : SDL_GetError()), ++logged;
}

}  // namespace

struct RawHid::Impl {
    std::thread thread;
    std::atomic<bool> running{false};
    HWND hwnd = nullptr;
    DWORD thread_id = 0;
    mutable std::mutex mu;
    std::map<HANDLE, Device> devices;
    uint32_t next_id = kIdBase;

    // 初めて見た機器の情報を集める。XInput の機器（Xbox のコントローラーなど）は SDL が読むので除く
    Device* device_of(HANDLE h) {
        auto it = devices.find(h);
        if (it != devices.end()) return it->second.preparsed.empty() ? nullptr : &it->second;
        Device& d = devices[h];
        UINT size = 0;
        GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &size);
        std::wstring path(size, L'\0');
        GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, path.data(), &size);
        if (path.find(L"IG_") != std::wstring::npos) return nullptr;
        size = 0;
        GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size);
        if (size == 0) return nullptr;
        std::vector<uint8_t> pp(size);
        if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, pp.data(), &size) == static_cast<UINT>(-1)) return nullptr;
        auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(pp.data());
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS) return nullptr;
        // ジョイスティック・ゲームパッド（と多軸のコントローラー）として名乗る機器だけ
        //（ノート PC のタッチパッドなども HID で軸を持っているので、名乗りで除く）
        if (caps.UsagePage != HID_USAGE_PAGE_GENERIC ||
            (caps.Usage != HID_USAGE_GENERIC_JOYSTICK && caps.Usage != HID_USAGE_GENERIC_GAMEPAD && caps.Usage != 0x08))
            return nullptr;
        // ボタンの数
        USHORT nb = caps.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> bcaps(nb);
        if (nb && HidP_GetButtonCaps(HidP_Input, bcaps.data(), &nb, ppd) == HIDP_STATUS_SUCCESS) {
            for (const HIDP_BUTTON_CAPS& b : bcaps) {
                if (b.UsagePage != HID_USAGE_PAGE_BUTTON) continue;
                int hi = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
                d.buttons = std::max(d.buttons, hi);
            }
        }
        // 軸とハット
        USHORT nv = caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> vcaps(nv);
        if (nv && HidP_GetValueCaps(HidP_Input, vcaps.data(), &nv, ppd) == HIDP_STATUS_SUCCESS) {
            std::vector<HIDP_VALUE_CAPS> sorted(vcaps.begin(), vcaps.begin() + nv);
            std::sort(sorted.begin(), sorted.end(), [](const HIDP_VALUE_CAPS& a, const HIDP_VALUE_CAPS& b) {
                USAGE ua = a.IsRange ? a.Range.UsageMin : a.NotRange.Usage;
                USAGE ub = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
                return ua < ub;
            });
            for (const HIDP_VALUE_CAPS& v : sorted) {
                if (v.UsagePage != HID_USAGE_PAGE_GENERIC) continue;
                USAGE u = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                if (u == HID_USAGE_GENERIC_HATSWITCH) {
                    d.values.push_back({u, v.LogicalMin, v.LogicalMax, -1});
                    d.hats = 1;
                } else if (u >= HID_USAGE_GENERIC_X && u <= HID_USAGE_GENERIC_WHEEL) {
                    // 論理値の範囲が符号なしのとき、LogicalMax が負に見えることがある
                    LONG lmax = v.LogicalMax;
                    if (lmax < v.LogicalMin && v.BitSize < 32) lmax = (1L << v.BitSize) - 1;
                    std::printf("RawHid: 軸 %d = usage 0x%x、論理値 %ld〜%ld（%d ビット）\n", d.axes, u, v.LogicalMin, lmax, v.BitSize);
                    d.values.push_back({u, v.LogicalMin, lmax, d.axes++, v.BitSize});
                }
            }
        }
        if (d.buttons == 0 && d.axes == 0) return nullptr;
        d.preparsed = std::move(pp);
        d.id = next_id++;
        pad_number(d.id);  // 見つけた順にパッドの番号を振る（キーコンフィグで機器を区別するため）
        d.button_state.assign(d.buttons, false);
        d.axis_state.assign(d.axes, 0);
        // 製品名
        HANDLE f = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            wchar_t buf[256] = {};
            if (HidD_GetProductString(f, buf, sizeof(buf))) d.name = narrow(buf);
            CloseHandle(f);
        }
        if (d.name.empty()) d.name = "HID game controller";
        return &d;
    }

    void on_input(HRAWINPUT hri, uint64_t ts) {
        UINT size = 0;
        GetRawInputData(hri, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<uint8_t> buf(size);
        if (GetRawInputData(hri, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
        auto ri = reinterpret_cast<RAWINPUT*>(buf.data());
        if (ri->header.dwType != RIM_TYPEHID) return;
        std::lock_guard<std::mutex> lock(mu);
        Device* d = device_of(ri->header.hDevice);
        if (!d) return;
        auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(d->preparsed.data());
        for (DWORD r = 0; r < ri->data.hid.dwCount; ++r) {
            PCHAR report = reinterpret_cast<PCHAR>(ri->data.hid.bRawData + r * ri->data.hid.dwSizeHid);
            ULONG rlen = ri->data.hid.dwSizeHid;
            // ボタン：押されているものの一覧から、変わったものだけをイベントにする
            std::vector<bool> now(d->buttons, false);
            ULONG n = static_cast<ULONG>(d->buttons);
            std::vector<USAGE> usages(n + 1);
            if (n && HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages.data(), &n, ppd, report, rlen) ==
                         HIDP_STATUS_SUCCESS) {
                for (ULONG i = 0; i < n; ++i)
                    if (usages[i] >= 1 && usages[i] <= d->buttons) now[usages[i] - 1] = true;
            }
            for (int b = 0; b < d->buttons; ++b) {
                if (now[b] == d->button_state[b]) continue;
                d->button_state[b] = now[b];
                SDL_Event e{};
                e.type = now[b] ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
                e.jbutton.which = d->id;
                e.jbutton.button = static_cast<Uint8>(b);
                e.jbutton.down = now[b];
                push_event(e, ts);
            }
            for (const ValueCap& v : d->values) {
                ULONG val = 0;
                if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, v.usage, &val, ppd, report, rlen) !=
                    HIDP_STATUS_SUCCESS)
                    continue;
                if (v.index < 0) {
                    // ハット：0〜7 の向き（範囲外は中央）
                    static const Uint8 dirs[8] = {SDL_HAT_UP, SDL_HAT_RIGHTUP, SDL_HAT_RIGHT, SDL_HAT_RIGHTDOWN,
                                                  SDL_HAT_DOWN, SDL_HAT_LEFTDOWN, SDL_HAT_LEFT, SDL_HAT_LEFTUP};
                    LONG k = static_cast<LONG>(val) - v.lmin;
                    Uint8 h = (k >= 0 && k < 8 && static_cast<LONG>(val) <= v.lmax) ? dirs[k] : SDL_HAT_CENTERED;
                    if (h == d->hat_state) continue;
                    d->hat_state = h;
                    SDL_Event e{};
                    e.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
                    e.jhat.which = d->id;
                    e.jhat.hat = 0;
                    e.jhat.value = h;
                    push_event(e, ts);
                    continue;
                }
                // 軸：論理値の範囲を -32768〜32767 に広げる
                double range = static_cast<double>(v.lmax) - v.lmin;
                // HidP_GetUsageValue は生のビット列を返すので、論理値の範囲が負を含むときは符号を広げる
                // （専用コントローラーの皿は -127〜127 のことがあり、そのままだと半周ごとに値が跳んで逆回転に見える）
                LONG sv = static_cast<LONG>(val);
                if (v.lmin < 0 && v.bits > 0 && v.bits < 32 && (val & (1UL << (v.bits - 1)))) sv -= 1L << v.bits;
                double t = range > 0 ? (static_cast<double>(sv) - v.lmin) / range : 0.5;
                int16_t a = static_cast<int16_t>(std::clamp(t * 65535.0 - 32768.0, -32768.0, 32767.0));
                if (a == d->axis_state[v.index]) continue;
                d->axis_state[v.index] = a;
                static int axis_logged = 0;
                if (dev_env("RD_DEBUG_INPUT") && axis_logged < 300)
                    std::printf("RawHid: 軸 %d 生 %lu → %d\n", v.index, val, a), ++axis_logged;
                SDL_Event e{};
                e.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
                e.jaxis.which = d->id;
                e.jaxis.axis = static_cast<Uint8>(v.index);
                e.jaxis.value = a;
                push_event(e, ts);
            }
        }
    }

    static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_INPUT && self) {
            static int logged = 0;
            if (dev_env("RD_DEBUG_INPUT") && logged < 10) std::printf("RawHid: WM_INPUT を受け取った（%d）\n", ++logged);
            self->on_input(reinterpret_cast<HRAWINPUT>(lp), SDL_GetTicksNS());
        } else if (msg == WM_INPUT_DEVICE_CHANGE && self && wp == GIDC_REMOVAL) {
            std::lock_guard<std::mutex> lock(self->mu);
            auto it = self->devices.find(reinterpret_cast<HANDLE>(lp));
            if (it != self->devices.end()) {
                if (!it->second.preparsed.empty()) pad_forget(it->second.id);  // パッドの番号を空ける
                self->devices.erase(it);
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void run() {
        thread_id = GetCurrentThreadId();
        // 入力の時刻の精度を上げるため、このスレッドの優先度を上げる
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        WNDCLASSW wc{};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RDRawHid";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // ジョイスティックとゲームパッド。ウィンドウが裏にあっても受け取る
        RAWINPUTDEVICE rid[2] = {
            {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
            {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd},
        };
        if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
            std::printf("RawHid: RegisterRawInputDevices に失敗した（%lu）\n", GetLastError());
        else
            std::printf("RawHid: ジョイスティック・ゲームパッドの Raw Input を登録した\n");
        // 最初からつながっている機器を先に調べておく（キーコンフィグ画面に名前を出すため）
        UINT count = 0;
        GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
        std::vector<RAWINPUTDEVICELIST> list(count);
        if (count && GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1)) {
            std::lock_guard<std::mutex> lock(mu);
            for (UINT i = 0; i < count; ++i)
                if (list[i].dwType == RIM_TYPEHID) device_of(list[i].hDevice);
        }
        running = true;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        DestroyWindow(hwnd);
        running = false;
    }
};

void RawHid::start() {
    if (impl_) return;
    impl_ = new Impl;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void RawHid::stop() {
    if (!impl_) return;
    while (!impl_->thread_id) std::this_thread::yield();
    PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    impl_->thread.join();
    delete impl_;
    impl_ = nullptr;
}

bool RawHid::running() const { return impl_ && impl_->running; }

std::vector<RawHidDevice> RawHid::devices() const {
    std::vector<RawHidDevice> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lock(impl_->mu);
    for (auto& [h, d] : impl_->devices) {
        if (d.preparsed.empty()) continue;
        out.push_back({d.id, d.name, d.buttons, d.axes, d.hats, d.axis_state});
    }
    std::sort(out.begin(), out.end(), [](const RawHidDevice& a, const RawHidDevice& b) { return a.id < b.id; });
    return out;
}

}  // namespace rd

#else

namespace rd {
struct RawHid::Impl {};
void RawHid::start() {}
void RawHid::stop() {}
bool RawHid::running() const { return false; }
std::vector<RawHidDevice> RawHid::devices() const { return {}; }
}  // namespace rd

#endif
