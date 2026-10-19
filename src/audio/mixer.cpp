#include "audio/mixer.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>

#include "miniaudio.h"

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#endif

namespace bmj {

Mixer::Mixer() = default;
Mixer::~Mixer() { close(); }

bool Mixer::open(uint32_t preferred_rate, uint32_t period_frames, bool exclusive) {
    device_ = std::make_unique<ma_device>();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.shareMode = exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = preferred_rate;  // 0ならデバイスの既定
    cfg.periodSizeInFrames = period_frames;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;
    cfg.noClip = MA_TRUE;
    cfg.dataCallback = &Mixer::callback;
    cfg.pUserData = this;
    ma_result res = ma_device_init(nullptr, &cfg, device_.get());
    if (res != MA_SUCCESS && exclusive) {
        // 排他モードが使えない（ほかのアプリが使っている・形式が合わない）ときは共有モードで開き直す
        cfg.playback.shareMode = ma_share_mode_shared;
        res = ma_device_init(nullptr, &cfg, device_.get());
    }
    if (res != MA_SUCCESS) {
        device_.reset();
        return false;
    }
    rate_ = device_->sampleRate;
    exclusive_ = device_->playback.shareMode == ma_share_mode_exclusive;
    period_frames_ = device_->playback.internalPeriodSizeInFrames;
    periods_ = device_->playback.internalPeriods;
    // 測定用のクリック音（1kHz を 5ms）
    test_click_.pcm.assign(static_cast<size_t>(rate_ / 200) * 2, 0.0f);
    for (size_t i = 0; i < test_click_.pcm.size() / 2; ++i)
        test_click_.pcm[i * 2] = test_click_.pcm[i * 2 + 1] = (i / (rate_ / 2000)) % 2 ? 0.5f : -0.5f;
    uint64_t buffered = static_cast<uint64_t>(device_->playback.internalPeriodSizeInFrames) *
                        std::max<ma_uint32>(1, device_->playback.internalPeriods);
    latency_us_ = static_cast<int64_t>(buffered * 1000000ull / device_->playback.internalSampleRate);
    if (ma_device_start(device_.get()) != MA_SUCCESS) {
        ma_device_uninit(device_.get());
        device_.reset();
        return false;
    }
    return true;
}

bool Mixer::device_position(double& played_sec, uint64_t& qpc_100ns) const {
#ifdef _WIN32
    if (!device_ || device_->pContext->backend != ma_backend_wasapi) return false;
    auto* client = static_cast<IAudioClient*>(device_->wasapi.pAudioClientPlayback);
    if (!client) return false;
    IAudioClock* clock = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock))) || !clock) return false;
    UINT64 freq = 0, pos = 0, qpc = 0;
    bool ok = SUCCEEDED(clock->GetFrequency(&freq)) && freq > 0 && SUCCEEDED(clock->GetPosition(&pos, &qpc));
    clock->Release();
    if (!ok) return false;
    played_sec = static_cast<double>(pos) / static_cast<double>(freq);
    qpc_100ns = qpc;
    return true;
#else
    (void)played_sec, (void)qpc_100ns;
    return false;
#endif
}

void Mixer::wait_callback() const {
    int64_t f0 = rendered_frames_.load(std::memory_order_relaxed);
    for (int i = 0; i < 200 && rendered_frames_.load(std::memory_order_relaxed) == f0; ++i) SDL_Delay(1);
    int64_t f1 = rendered_frames_.load(std::memory_order_relaxed);
    for (int i = 0; i < 200 && rendered_frames_.load(std::memory_order_relaxed) == f1; ++i) SDL_Delay(1);
}

bool Mixer::calibrate_latency() {
#ifdef _WIN32
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    double sum = 0;
    int n = 0;
    for (int i = 0; i < 40; ++i) {
        double played = 0;
        uint64_t qpc = 0;
        int64_t rendered = rendered_frames_.load(std::memory_order_relaxed);
        if (!device_position(played, qpc)) return false;
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        double now_100ns = static_cast<double>(c.QuadPart) * 1e7 / static_cast<double>(qf.QuadPart);
        // いま鳴っている位置（位置を取ってから今までに進んだ分を足す）
        double playing = played + (now_100ns - static_cast<double>(qpc)) / 1e7;
        sum += static_cast<double>(rendered) / rate_ - playing;
        ++n;
        SDL_DelayNS(1700000);  // 周期とそろわないように半端な間隔で取る
    }
    double ahead = sum / n - (period_frames_ / 2.0) / rate_;
    if (ahead <= 0 || ahead > 0.5) return false;
    latency_us_ = static_cast<int64_t>(ahead * 1e6);
    return true;
#else
    return false;
#endif
}

void Mixer::close() {
    if (!device_) return;
    ma_device_uninit(device_.get());
    device_.reset();
}

void Mixer::set_song(const std::vector<Sample>* samples, std::vector<ScheduledSound> schedule) {
    std::sort(schedule.begin(), schedule.end(),
              [](const ScheduledSound& a, const ScheduledSound& b) { return a.frame < b.frame; });
    samples_ = samples;
    schedule_ = std::move(schedule);
    schedule_next_ = 0;
}

void Mixer::start(int64_t lead_in_frames) {
    // 音声スレッドは running_ が立つまで曲のボイスを読まないので、ここで初期化してよい
    for (Voice& v : voices_) v = Voice{};
    song_frame_ = -lead_in_frames;
    schedule_next_ = 0;
    clock_valid_ = false;
    clock_seq_.fetch_add(1);
    base_us_.store(-lead_in_frames * 1000000 / rate_);
    base_wall_ns_.store(SDL_GetTicksNS());
    clock_seq_.fetch_add(1);
    running_.store(true, std::memory_order_release);
}

void Mixer::stop() { running_.store(false, std::memory_order_release); }

void Mixer::push_command(int32_t cmd) {
    size_t tail = queue_tail_.load(std::memory_order_relaxed);
    size_t next = (tail + 1) % kQueueSize;
    if (next == queue_head_.load(std::memory_order_acquire)) return;  // 満杯なら捨てる
    queue_[tail] = cmd;
    queue_tail_.store(next, std::memory_order_release);
}

int64_t Mixer::song_time_us_at(uint64_t wall_ns) const {
    int64_t base;
    uint64_t wall;
    uint64_t s0, s1;
    do {
        s0 = clock_seq_.load(std::memory_order_acquire);
        base = base_us_.load(std::memory_order_relaxed);
        wall = base_wall_ns_.load(std::memory_order_relaxed);
        s1 = clock_seq_.load(std::memory_order_acquire);
    } while (s0 != s1 || (s0 & 1));
    int64_t dt_us = (static_cast<int64_t>(wall_ns) - static_cast<int64_t>(wall)) / 1000;
    return base + dt_us - latency_us_;
}

void Mixer::callback(ma_device* dev, void* out, const void*, unsigned int frames) {
    static_cast<Mixer*>(dev->pUserData)->render(static_cast<float*>(out), frames);
}

void Mixer::start_voice(uint16_t sound, int64_t delay_frames, bool key) {
    if (!samples_ || sound >= samples_->size()) return;
    const Sample& s = (*samples_)[sound];
    if (s.pcm.empty()) return;
    // 同じキー音が鳴っている最中に鳴らし直したら、前のものは止める
    for (Voice& v : voices_) {
        if (v.sample && v.sound == sound) v.sample = nullptr;
    }
    Voice* slot = nullptr;
    for (Voice& v : voices_) {
        if (!v.sample) {
            slot = &v;
            break;
        }
    }
    if (!slot) {
        // 空きが無ければ、一番長く鳴っているものを止める
        slot = &voices_[0];
        for (Voice& v : voices_)
            if (v.pos > slot->pos) slot = &v;
    }
    slot->sample = &s;
    slot->sound = sound;
    slot->pos = -delay_frames;
    slot->key = key;
}

void Mixer::run_system_command(int32_t cmd) {
    int c = cmd - kSysBase;
    int id = c & 0xff;
    if (c & kSysStop) {
        for (SysVoice& v : sys_voices_)
            if (v.sample && v.id == id) v.sample = nullptr;
        return;
    }
    if (id >= static_cast<int>(sys_samples_.size()) || sys_samples_[id].pcm.empty()) return;
    // 同じ音を鳴らし直したら前のものは止める（キー音と同じ）
    SysVoice* slot = nullptr;
    for (SysVoice& v : sys_voices_) {
        if (v.sample && v.id == id) v.sample = nullptr;
        if (!v.sample && !slot) slot = &v;
    }
    if (!slot) slot = &sys_voices_[0];
    slot->sample = &sys_samples_[id];
    slot->pos = 0;
    slot->id = id;
    slot->loop = c & kSysLoop;
}

void Mixer::update_clock(uint64_t wall_ns) {
    int64_t raw_us = song_frame_ * 1000000 / rate_;
    if (!clock_valid_) {
        est_us_ = raw_us;
        est_wall_ = wall_ns;
        clock_valid_ = true;
    } else {
        int64_t predicted = est_us_ + static_cast<int64_t>(wall_ns - est_wall_) / 1000;
        int64_t err = raw_us - predicted;
        // コールバックの揺れを均すため、ずれの1/16ずつ寄せる。大きくずれたら合わせ直す
        if (std::llabs(err) > 20000) predicted = raw_us;
        else predicted += err / 16;
        est_us_ = predicted;
        est_wall_ = wall_ns;
    }
    clock_seq_.fetch_add(1, std::memory_order_acq_rel);
    base_us_.store(est_us_, std::memory_order_relaxed);
    base_wall_ns_.store(est_wall_, std::memory_order_relaxed);
    clock_seq_.fetch_add(1, std::memory_order_acq_rel);
}

void Mixer::render(float* out, uint32_t frames) {
    std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);
    bool running = running_.load(std::memory_order_acquire);

    // メインスレッドからの命令（キー押下の音・システム音）
    size_t head = queue_head_.load(std::memory_order_relaxed);
    size_t tail = queue_tail_.load(std::memory_order_acquire);
    while (head != tail) {
        int32_t cmd = queue_[head];
        if (cmd == kTestClick) {
            test_click_pos_ = 0;
            click_frame_.store(rendered_frames_.load(std::memory_order_relaxed), std::memory_order_release);
        } else if (cmd >= kSysBase) {
            run_system_command(cmd);
        } else if (!running) {
            // 曲を止めている間のキー音は捨てる
        } else if (cmd >= 0) {
            start_voice(static_cast<uint16_t>(cmd), 0, true);
        } else {
            uint16_t id = static_cast<uint16_t>(-cmd - 1);
            for (Voice& v : voices_)
                if (v.sample && v.sound == id) v.sample = nullptr;
        }
        head = (head + 1) % kQueueSize;
    }
    queue_head_.store(head, std::memory_order_release);

    // 測定用のクリック音
    if (test_click_pos_ >= 0) {
        int64_t len = test_click_.frames();
        for (uint32_t i = 0; i < frames && test_click_pos_ < len; ++i, ++test_click_pos_) {
            out[i * 2] += test_click_.pcm[test_click_pos_ * 2];
            out[i * 2 + 1] += test_click_.pcm[test_click_pos_ * 2 + 1];
        }
        if (test_click_pos_ >= len) test_click_pos_ = -1;
    }
    rendered_frames_.fetch_add(frames, std::memory_order_relaxed);
    // システム音。くり返し鳴らすもの（選曲 BGM）は、プレビュー中に音量を下げる
    const float loop_target = sys_loop_target_.load(std::memory_order_relaxed);
    const float loop_step = sys_loop_step_.load(std::memory_order_relaxed);
    const float loop_gain0 = sys_loop_gain_;
    auto loop_gain_at = [&](uint32_t i) {
        float d = loop_step * static_cast<float>(i + 1);
        return loop_gain0 < loop_target ? std::min(loop_target, loop_gain0 + d) : std::max(loop_target, loop_gain0 - d);
    };
    sys_loop_gain_ = frames ? loop_gain_at(frames - 1) : loop_gain0;
    // 全体のフェード（リザルトを抜けるときなど）と、KEY 側・BGM 側の音量
    const float fade_target = fade_target_.load(std::memory_order_relaxed);
    const float fade_step = fade_step_.load(std::memory_order_relaxed);
    const float fade0 = fade_gain_;
    auto fade_at = [&](uint32_t i) {
        float d = fade_step * static_cast<float>(i + 1);
        return fade0 < fade_target ? std::min(fade_target, fade0 + d) : std::max(fade_target, fade0 - d);
    };
    fade_gain_ = frames ? fade_at(frames - 1) : fade0;
    const float gk = gain_key_.load(std::memory_order_relaxed), gb = gain_bgm_.load(std::memory_order_relaxed);
    const uint32_t key_mask = sys_key_mask_.load(std::memory_order_relaxed);
    for (SysVoice& v : sys_voices_) {
        if (!v.sample) continue;
        const float* pcm = v.sample->pcm.data();
        int64_t len = v.sample->frames();
        const float side = v.id < 32 && (key_mask >> v.id & 1u) ? gk : gb;
        for (uint32_t i = 0; i < frames; ++i) {
            if (v.pos >= len) {
                if (!v.loop) break;
                v.pos = 0;
            }
            float g = (v.loop ? loop_gain_at(i) : 1.0f) * side * fade_at(i);
            out[i * 2] += pcm[v.pos * 2] * g;
            out[i * 2 + 1] += pcm[v.pos * 2 + 1] * g;
            ++v.pos;
        }
        if (!v.loop && v.pos >= len) v.sample = nullptr;
    }
    if (!running) {
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        return;
    }
    update_clock(SDL_GetTicksNS());

    // この区間に入る予定の音（BGM・自動演奏）を、フレーム単位の位置で鳴らす
    int64_t end = song_frame_ + frames;
    while (schedule_next_ < schedule_.size() && schedule_[schedule_next_].frame < end) {
        const ScheduledSound& e = schedule_[schedule_next_++];
        int64_t delay = std::max<int64_t>(0, e.frame - song_frame_);
        start_voice(e.sound, delay, e.key);
    }

    int active = 0;
    for (Voice& v : voices_) {
        if (!v.sample) continue;
        const float* pcm = v.sample->pcm.data();
        int64_t len = v.sample->frames();
        int64_t i0 = 0;
        if (v.pos < 0) i0 = std::min<int64_t>(frames, -v.pos);
        const float side = v.key ? gk : gb;
        for (int64_t i = i0; i < frames; ++i) {
            int64_t p = v.pos + i;
            if (p >= len) break;
            float g = side * fade_at(static_cast<uint32_t>(i));
            out[i * 2] += pcm[p * 2] * g;
            out[i * 2 + 1] += pcm[p * 2 + 1] * g;
        }
        v.pos += frames;
        if (v.pos >= len) v.sample = nullptr;
        else ++active;
    }
    active_voices_.store(active, std::memory_order_relaxed);

    for (uint32_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
    song_frame_ = end;
}

}  // namespace bmj
