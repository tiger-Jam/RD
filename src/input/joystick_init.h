// ジョイスティックの準備を裏で行う。Windowsでは DirectInput の列挙に数十秒かかる環境がある
// （Logicool G HUB の仮想デバイスなど）ので、その間も画面を止めない
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <thread>

namespace bmj {

class JoystickInit {
public:
    void start() {
        auto init = [this] {
            uint64_t t0 = SDL_GetTicksNS();
            ok_ = SDL_InitSubSystem(SDL_INIT_JOYSTICK);
            ms_ = static_cast<int>((SDL_GetTicksNS() - t0) / 1000000);
            ready_ = true;
        };
#if defined(_WIN32)
        thread_ = std::thread(init);
#else
        init();  // macOS のコントローラーはメインスレッドで準備する必要がある
#endif
    }
    bool ready() const { return ready_.load(); }
    bool ok() const { return ok_.load(); }
    int took_ms() const { return ms_.load(); }
    // 終了する前に呼ぶ（準備中に SDL_Quit すると落ちるため）
    void join() {
        if (thread_.joinable()) thread_.join();
    }

private:
    std::thread thread_;
    std::atomic<bool> ready_{false}, ok_{false};
    std::atomic<int> ms_{0};
};

// アプリ全体で1つ
JoystickInit& joystick_init();

}  // namespace bmj
