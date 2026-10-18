// 音声出力と自前ミキサー、オーディオクロック（仕様書4章・6章）
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/sound_bank.h"

struct ma_device;

namespace bmj {

struct ScheduledSound {
    int64_t frame = 0;  // 譜面の時刻0からのフレーム位置
    uint16_t sound = 0;
};

class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    bool open(uint32_t preferred_rate, uint32_t period_frames);
    void close();
    uint32_t sample_rate() const { return rate_; }
    // 出力までの遅延の推定値（マイクロ秒）
    int64_t output_latency_us() const { return latency_us_; }

    // 再生前に、キー音と予定表（BGMや自動演奏）を渡す。再生中は呼ばないこと
    void set_song(const std::vector<Sample>* samples, std::vector<ScheduledSound> schedule);
    // lead_in_frames 後に譜面の時刻0が来るように再生を始める
    void start(int64_t lead_in_frames);
    void stop();

    // 今すぐ鳴らす／止める（キー押下・LNを離したとき）。メインスレッドからだけ呼ぶ
    void play_now(uint16_t sound) { push_command(static_cast<int32_t>(sound)); }
    void stop_now(uint16_t sound) { push_command(-static_cast<int32_t>(sound) - 1); }

    // 譜面上の時刻（マイクロ秒）。wall_ns は SDL_GetTicksNS() と同じ基準の時刻。
    // スピーカーから実際に出ている位置を返す（出力遅延を差し引く）
    int64_t song_time_us_at(uint64_t wall_ns) const;
    bool running() const { return running_.load(std::memory_order_acquire); }
    // 鳴っているボイスがあるか（曲の終了判定用）
    bool any_voice_active() const { return active_voices_.load(std::memory_order_relaxed) > 0; }

private:
    static void callback(ma_device* dev, void* out, const void* in, unsigned int frames);
    void render(float* out, uint32_t frames);
    void push_command(int32_t cmd);
    void start_voice(uint16_t sound, int64_t delay_frames);
    void update_clock(uint64_t wall_ns);

    struct Voice {
        const Sample* sample = nullptr;
        int64_t pos = 0;  // 再生位置（負なら、その分だけ遅れて始まる）
        uint16_t sound = 0;
    };

    std::unique_ptr<ma_device> device_;
    uint32_t rate_ = 48000;
    int64_t latency_us_ = 0;

    const std::vector<Sample>* samples_ = nullptr;
    std::vector<ScheduledSound> schedule_;
    size_t schedule_next_ = 0;
    std::array<Voice, 256> voices_{};  // LR2（FMOD）と同じ256
    std::atomic<int> active_voices_{0};

    // メインスレッド → 音声スレッドのキュー（単一生産者・単一消費者）
    static constexpr size_t kQueueSize = 1024;
    std::array<int32_t, kQueueSize> queue_{};  // 0以上なら再生、負なら -(id+1) の停止
    std::atomic<size_t> queue_head_{0}, queue_tail_{0};

    std::atomic<bool> running_{false};
    int64_t song_frame_ = 0;  // 次に書き出すフレームの譜面上の位置（音声スレッドだけが触る）

    // 平滑化したクロック。base_wall_ns の時点で譜面の時刻が base_us だった、という組
    bool clock_valid_ = false;
    int64_t est_us_ = 0;
    uint64_t est_wall_ = 0;
    std::atomic<uint64_t> clock_seq_{0};
    std::atomic<int64_t> base_us_{0};
    std::atomic<uint64_t> base_wall_ns_{0};
};

}  // namespace bmj
