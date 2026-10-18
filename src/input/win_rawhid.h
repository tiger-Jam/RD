// Windows のコントローラーを Raw Input で直接読む（仕様書6章「Windowsでは専用のRawInputスレッドに差し替えられるようにする」）。
// SDL は専用コントローラー（INFINITAS 専用コントローラーなど）を DirectInput でしか読めず、
// DirectInput の機器の列挙は環境によって数十秒かかる（Logicool G HUB の仮想デバイスなど）ので、それを避ける。
// 読んだ入力は SDL のジョイスティックのイベントとして送るので、InputMapper などはそのまま使える。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bmj {

struct RawHidDevice {
    uint32_t id = 0;  // SDL のイベントの which に入る番号
    std::string name;
    int buttons = 0, axes = 0, hats = 0;
    std::vector<int16_t> axis_values;
};

class RawHid {
public:
    // 専用のスレッドを立てて読み始める。Windows 以外では何もしない
    void start();
    void stop();
    bool running() const;
    std::vector<RawHidDevice> devices() const;
    ~RawHid() { stop(); }

    struct Impl;

private:
    [[maybe_unused]] Impl* impl_ = nullptr;
};

RawHid& raw_hid();

}  // namespace bmj
