#include "audio/mixer.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>

#include "miniaudio.h"

namespace bmj {

Mixer::Mixer() = default;
Mixer::~Mixer() { close(); }

bool Mixer::open(uint32_t preferred_rate, uint32_t period_frames) {
    device_ = std::make_unique<ma_device>();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = preferred_rate;  // 0ならデバイスの既定
    cfg.periodSizeInFrames = period_frames;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;
    cfg.noClip = MA_TRUE;
    cfg.dataCallback = &Mixer::callback;
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, device_.get()) != MA_SUCCESS) {
        device_.reset();
        return false;
    }
    rate_ = device_->sampleRate;
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
    // 音声スレッドは running_ が立つまで何も読まないので、ここで初期化してよい
    for (Voice& v : voices_) v = Voice{};
    queue_head_.store(queue_tail_.load());
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

void Mixer::start_voice(uint16_t sound, int64_t delay_frames) {
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
    if (!running_.load(std::memory_order_acquire)) return;

    update_clock(SDL_GetTicksNS());

    // キー押下の音
    size_t head = queue_head_.load(std::memory_order_relaxed);
    size_t tail = queue_tail_.load(std::memory_order_acquire);
    while (head != tail) {
        int32_t cmd = queue_[head];
        if (cmd >= 0) {
            start_voice(static_cast<uint16_t>(cmd), 0);
        } else {
            uint16_t id = static_cast<uint16_t>(-cmd - 1);
            for (Voice& v : voices_)
                if (v.sample && v.sound == id) v.sample = nullptr;
        }
        head = (head + 1) % kQueueSize;
    }
    queue_head_.store(head, std::memory_order_release);

    // この区間に入る予定の音（BGM・自動演奏）を、フレーム単位の位置で鳴らす
    int64_t end = song_frame_ + frames;
    while (schedule_next_ < schedule_.size() && schedule_[schedule_next_].frame < end) {
        const ScheduledSound& e = schedule_[schedule_next_++];
        int64_t delay = std::max<int64_t>(0, e.frame - song_frame_);
        start_voice(e.sound, delay);
    }

    int active = 0;
    for (Voice& v : voices_) {
        if (!v.sample) continue;
        const float* pcm = v.sample->pcm.data();
        int64_t len = v.sample->frames();
        int64_t i0 = 0;
        if (v.pos < 0) i0 = std::min<int64_t>(frames, -v.pos);
        for (int64_t i = i0; i < frames; ++i) {
            int64_t p = v.pos + i;
            if (p >= len) break;
            out[i * 2] += pcm[p * 2];
            out[i * 2 + 1] += pcm[p * 2 + 1];
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
