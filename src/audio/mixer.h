// 音声出力と自前ミキサー、オーディオクロック（仕様書4章・6章）
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/dsp.h"
#include "audio/sound_bank.h"

struct ma_device;

namespace bmj {

struct ScheduledSound {
    int64_t frame = 0;  // 譜面の時刻0からのフレーム位置
    uint16_t sound = 0;
    bool key = false;   // キー音（自動演奏）として鳴らす（音量は KEY 側。false なら BGM 側）
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
    // 予定の音（BGM・自動演奏）をこれ以上鳴らし始めない。鳴っている音はそのまま最後まで鳴らす
    // （LR2：演奏を終えてフェードアウトに入ったら、新しい音は鳴らさないが、鳴っている音は止めずにリザルトへ持ち込む）
    void freeze_schedule() { schedule_frozen_.store(true, std::memory_order_release); }

    // 今すぐ鳴らす／止める（キー押下・LNを離したとき）。メインスレッドからだけ呼ぶ
    void play_now(uint16_t sound) { push_command(static_cast<int32_t>(sound)); }  // キー音（KEY 側）
    void stop_now(uint16_t sound) { push_command(-static_cast<int32_t>(sound) - 1); }

    // システム音（決定音・選曲BGMなど）。曲を止めていても鳴る。
    // 鳴らし始めたあとに替えるときは、鳴っているシステム音を全部止め、コールバックが止めたのを待ってから差し替える
    void set_system_sounds(std::vector<Sample> sounds) {
        if (!sys_samples_.empty()) {
            for (int id = 0; id < static_cast<int>(sys_samples_.size()); ++id) stop_system(id);
            wait_callback();
            wait_callback();
        }
        sys_samples_ = std::move(sounds);
    }
    void play_system(int id, bool loop = false) { push_command(kSysBase + id + (loop ? kSysLoop : 0)); }
    void stop_system(int id) { push_command(kSysBase + id + kSysStop); }
    // 音量（LR2: volumeflag / master / key / bgm。0〜100）。on が false なら音量の設定を使わない（全部 100）。
    // キー音と KEY 側のシステム音は MASTER×KEY、BGM と BGM 側のシステム音は MASTER×BGM
    void set_volume(bool on, int master, int key, int bgm) {
        gain_key_.store(on ? master / 100.0f * key / 100.0f : 1.0f);
        gain_bgm_.store(on ? master / 100.0f * bgm / 100.0f : 1.0f);
    }
    // システム音のうち KEY 側のもの（ビットが id）。それ以外は BGM 側
    void set_system_key_side(uint32_t mask) { sys_key_mask_.store(mask); }
    // 全体の音量を ms かけて target にする（LR2: リザルトを抜けるときのフェードアウト）。1 で元に戻す
    void fade_all(float target, int ms) {
        fade_target_.store(target);
        fade_step_.store(ms > 0 ? 1.0f / (ms * 0.001f * static_cast<float>(rate_)) : 1.0f);
    }
    // イコライザ・ピッチ・エフェクタ（LR2: ApplySoundFX）。メインスレッドからだけ呼ぶ。音声スレッドは次の周期で使う
    void set_sound_fx(const SoundFx& fx);
    // 今の再生の速さ（FREQ・SPEED。曲の時計の進む速さ）
    double play_rate() const { return clock_rate_.load(std::memory_order_relaxed); }
    // くり返し鳴らしているシステム音（選曲 BGM）の音量を、ms かけて target にする（LR2: SetFadePreview）
    void fade_system_loop(float target, int ms) {
        sys_loop_target_.store(target);
        sys_loop_step_.store(ms > 0 ? 1.0f / (ms * 0.001f * static_cast<float>(rate_)) : 1.0f);
    }
    // 音声のコールバックが少なくとも 1 回まわるまで待つ（stop() のあと、曲の音を差し替えてよくなるまで）
    void wait_callback() const;
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
    // 1 区間（kChunk フレームまで）を鳴らす
    void render_chunk(float* out, uint32_t n, bool running);
    void apply_sound_fx(const SoundFx& fx);
    void push_command(int32_t cmd);
    // pos は始めの再生位置（負なら、その分だけ譜面の時刻で遅れて始まる）
    void start_voice(uint16_t sound, double pos, bool key);
    void update_clock(uint64_t wall_ns);

    struct Voice {
        const Sample* sample = nullptr;
        double pos = 0;  // 再生位置（フレーム。負なら、その分だけ遅れて始まる）
        uint16_t sound = 0;
        bool key = false;  // KEY 側の音量を使う
    };
    struct SysVoice {
        const Sample* sample = nullptr;
        double pos = 0;
        int id = 0;
        bool loop = false;
    };
    static constexpr int32_t kSysBase = 0x10000, kSysLoop = 0x100, kSysStop = 0x200;
    static constexpr int32_t kTestClick = 0x7fff0000;
    Sample test_click_;
    int64_t test_click_pos_ = -1;
    std::atomic<int64_t> rendered_frames_{0};  // これまでに書き出したフレームの数（音声スレッドだけが増やす）
    std::atomic<int64_t> click_frame_{-1};
    std::atomic<float> sys_loop_target_{1.0f}, sys_loop_step_{1.0f};
    float sys_loop_gain_ = 1.0f;  // 音声スレッドだけが触る
    std::atomic<float> gain_key_{1.0f}, gain_bgm_{1.0f};
    std::atomic<uint32_t> sys_key_mask_{0};
    std::atomic<float> fade_target_{1.0f}, fade_step_{1.0f};
    float fade_gain_ = 1.0f;  // 音声スレッドだけが触る
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
    // 次に書き出すフレームの譜面上の位置（譜面の時刻のフレーム。FREQ・SPEED なら 1 フレームで play_rate_ 進む）。
    // 音声スレッドだけが触る
    std::atomic<bool> schedule_frozen_{false};
    double song_pos_ = 0;

    // イコライザ・ピッチ・エフェクタ。設定はメインスレッドから 3 つの枠で受け渡す（ロックを使わない）
    std::array<SoundFx, 3> fx_slots_{};
    int fx_write_ = 0, fx_read_ = 1;    // それぞれメインスレッド・音声スレッドだけが触る
    std::atomic<int> fx_mid_{2};        // 下位 2 ビットが受け渡し中の枠、4 が「新しい」の印
    double play_rate_ = 1.0;            // 音声スレッドだけが触る
    std::atomic<int64_t> shift_latency_us_{0};  // 音程だけを変えるときに増える遅れ
    Equalizer eq_;
    std::array<Effector, 3> effector_{};
    PitchShifter shifter_;
    static constexpr uint32_t kChunk = 512;
    std::vector<float> key_bus_, bgm_bus_;  // KEY 側・BGM 側の音を混ぜる所（kChunk フレーム）
    std::array<float, kChunk> fade_curve_{}, loop_curve_{};

    // 平滑化したクロック。base_wall_ns の時点で譜面の時刻が base_us だった、という組
    bool clock_valid_ = false;
    int64_t est_us_ = 0;
    uint64_t est_wall_ = 0;
    std::atomic<uint64_t> clock_seq_{0};
    std::atomic<int64_t> base_us_{0};
    std::atomic<uint64_t> base_wall_ns_{0};
    std::atomic<double> clock_rate_{1.0};
};

}  // namespace bmj
