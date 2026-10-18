// ジョイスティックの準備を裏で行う。Windowsでは DirectInput の列挙に数十秒かかる環境がある
// （Logicool G HUB の仮想デバイスなど）ので、その間も画面を止めない
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>
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
        // 準備中はイベントの受け取りでジョイスティックを更新しない（準備のロックを待ってメインスレッドが止まるため）
        SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
        thread_ = std::thread([this, init] {
            init();
            SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1");
        });
#else
        init();  // macOS のコントローラーはメインスレッドで準備する必要がある
#endif
    }
    bool ready() const { return ready_.load(); }
    bool ok() const { return ok_.load(); }
    int took_ms() const { return ms_.load(); }
    // つながっているコントローラーを開く。準備を裏で行うと、最初からつながっている分の
    // SDL_EVENT_JOYSTICK_ADDED がメインスレッドに届かないので、準備が終わったら自分で一覧して開く。
    // 新しく開いたものの名前を返す（メインスレッドから毎フレーム呼んでよい）
    std::vector<std::string> open_new() {
        std::vector<std::string> names;
        if (!ready_ || !ok_) return names;
        int n = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&n);
        for (int i = 0; ids && i < n; ++i) {
            if (SDL_GetJoystickFromID(ids[i])) continue;  // 開いている
            if (SDL_Joystick* j = SDL_OpenJoystick(ids[i])) {
                const char* name = SDL_GetJoystickName(j);
                names.push_back(name ? name : "?");
            }
        }
        SDL_free(ids);
        return names;
    }
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
