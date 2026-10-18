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

    // exclusive：Windows の WASAPI 排他モードを試す（使えなければ共有モードで開く）
    bool open(uint32_t preferred_rate, uint32_t period_frames, bool exclusive = false);
    bool exclusive() const { return exclusive_; }
    // 実際に使っている周期の長さと個数（フレーム）
    uint32_t period_frames() const { return period_frames_; }
    uint32_t periods() const { return periods_; }

    // 遅延の測定用（--latency-test）：短いクリック音を今すぐ鳴らす。鳴り始めのフレーム位置は click_frame() に入る
    void play_test_click() { push_command(kTestClick); }
    int64_t click_frame() const { return click_frame_.load(std::memory_order_acquire); }
    // オーディオ機器が今どこまで鳴らしたか（秒）と、その時刻（QueryPerformanceCounter と同じ基準の 100ns 単位）。
    // Windows（WASAPI）だけ。取れなければ false
    bool device_position(double& played_sec, uint64_t& qpc_100ns) const;
    // 出力の遅延を実測で合わせ直す（Windows のみ。数十ms かかる）。書き出したのにまだ鳴っていない長さの平均から、
    // コールバックの周期の半分を引いたものを遅延とする。測れたら true
    bool calibrate_latency();
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

    // システム音（決定音・選曲BGMなど）。曲を止めていても鳴る。set_system_sounds は鳴らし始める前に1回だけ呼ぶ
    void set_system_sounds(std::vector<Sample> sounds) { sys_samples_ = std::move(sounds); }
    void play_system(int id, bool loop = false) { push_command(kSysBase + id + (loop ? kSysLoop : 0)); }
    void stop_system(int id) { push_command(kSysBase + id + kSysStop); }
    // そのシステム音がサウンドセットにあるか（無いときは別の音で代わりに鳴らす）
    bool has_system(int id) const { return id >= 0 && id < static_cast<int>(sys_samples_.size()) && !sys_samples_[id].pcm.empty(); }

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
    struct SysVoice {
        const Sample* sample = nullptr;
        int64_t pos = 0;
        int id = 0;
        bool loop = false;
    };
    static constexpr int32_t kSysBase = 0x10000, kSysLoop = 0x100, kSysStop = 0x200;
    static constexpr int32_t kTestClick = 0x7fff0000;
    Sample test_click_;
    int64_t test_click_pos_ = -1;
    std::atomic<int64_t> rendered_frames_{0};  // これまでに書き出したフレームの数（音声スレッドだけが増やす）
    std::atomic<int64_t> click_frame_{-1};
    bool exclusive_ = false;
    uint32_t period_frames_ = 0, periods_ = 0;
    std::vector<Sample> sys_samples_;
    std::array<SysVoice, 16> sys_voices_{};
    void run_system_command(int32_t cmd);

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
    std::array<int32_t, kQueueSize> queue_{};  // 0以上なら再生、負なら -(id+1) の停止、kSysBase 以上はシステム音
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
