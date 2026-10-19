// 音の効果（イコライザ・エフェクタ・ピッチ）と、ミキサーの中の計算のうち音声デバイスに依存しないもの。
// ミキサーの音声スレッドから使う。prepare() のほかはメモリを確保しない（テストからも使う）
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace bmj {

// エフェクタの種類（LR2: fxtype。スキンのボタン 20〜22 のコマもこの並び）
enum FxType { kFxOff = 0, kFxReverb, kFxEcho, kFxLowPass, kFxHighPass, kFxFlanger, kFxChorus, kFxDistortion, kFxTypeCount };
// エフェクタの掛け先（LR2: fxtarget。ボタン 26〜28）。KEY はキー音と KEY 側のシステム音、BGM はそれ以外
enum FxTarget { kFxMaster = 0, kFxKey, kFxBgm };
// ピッチの方式（LR2: pitchtype。ボタン 33）。FREQ = 回転数（音程と速さ）、PITCH = 音程だけ、SPEED = 速さだけ
enum PitchType { kPitchFreq = 0, kPitchPitch, kPitchSpeed };

struct FxSlot {
    bool on = false;
    int type = kFxOff;
    int target = kFxMaster;
    int p1 = 0, p2 = 0;  // パラメータ 0〜100（スライダー 20〜25）
    bool operator==(const FxSlot&) const = default;
};

// イコライザ・ピッチ・エフェクタの設定（LR2: <sound> の eqflag / eqp0〜6 / pitchflag / pitchp / pitchtype / fx*_0〜2）
struct SoundFx {
    bool eq_on = false;
    std::array<int, 7> eq{};  // 63 / 160 / 400 / 1k / 2.5k / 6.3k / 16k Hz の増減（dB。-12〜12。スライダー 10〜16）
    bool pitch_on = false;
    int pitch = 0;  // 半音（-12〜12。スライダー 26）
    int pitch_type = kPitchFreq;
    std::array<FxSlot, 3> fx{};
    bool operator==(const SoundFx&) const = default;

    // 再生の速さ（FREQ と SPEED）。曲の時計もこの倍率で進む。PITCH やオフなら 1
    double play_rate() const;
    // 速さを変えずに音程だけを変える倍率（PITCH は 2^(n/12)。SPEED は回転数で上がった音程を戻す 2^(-n/12)）
    double shift_ratio() const;
    // 曲の速さが変わる（FREQ・SPEED で 0 以外）
    bool changes_speed() const { return play_rate() != 1.0; }
    // エフェクタの種類を変える。パラメータは LPF なら 100 / 0（開いた状態）、それ以外は 0 / 0 に戻す
    void set_fx_type(int slot, int type);
    // 範囲の外の値を収める（設定ファイルを読んだあと）
    void clamp();
};

// 半音 n 個分の倍率
double semitone_ratio(int n);

// 2 次の IIR フィルタ（ステレオ）
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z[2][2]{};
    void identity() { b0 = 1, b1 = b2 = a1 = a2 = 0; }
    void peaking(double fs, double f, double q, double db);
    void lowpass(double fs, double f, double q);
    void highpass(double fs, double f, double q);
    void reset() { z[0][0] = z[0][1] = z[1][0] = z[1][1] = 0; }
    void process(float* lr, uint32_t n);
};

// 7 バンドのイコライザ（ピーキング、Q=1.5）
class Equalizer {
public:
    static constexpr std::array<double, 7> kFreq{63, 160, 400, 1000, 2500, 6300, 16000};
    // gains が全部 0 なら何もしない
    void set(double fs, const std::array<int, 7>& gains);
    void process(float* lr, uint32_t n);
    bool flat() const { return flat_; }

private:
    std::array<Biquad, 7> band_{};
    std::array<bool, 7> on_{};
    bool flat_ = true;
};

// エフェクタ 1 つ分。種類ごとの状態を全部持ち、選んだ種類だけを動かす
class Effector {
public:
    // 遅延用のバッファを確保する（音声スレッドが動く前に呼ぶ）
    void prepare(double fs);
    // 設定を移す。種類が変わったときと、オフからオンにしたときは中の音を消す
    void set(const FxSlot& s);
    bool active() const { return active_; }
    int target() const { return target_; }
    void process(float* lr, uint32_t n);
    void reset();

private:
    double fs_ = 48000;
    bool active_ = false;
    int type_ = kFxOff, target_ = kFxMaster;
    // 種類ごとのパラメータ（LR2 のスライダーの値から換算したもの）
    double a_ = 0, b_ = 0;
    // 遅延線（エコー・フランジャー・コーラス）。左右を交互に並べる
    std::vector<float> line_;
    size_t line_mask_ = 0, line_pos_ = 0;
    double lfo_ = 0;
    Biquad filter_;
    // リバーブ（くし形 8 本と全域通過 4 本を左右に）
    struct Delay {
        size_t off = 0, len = 1, pos = 0;
        float store = 0;  // くし形の中の低域通過
        float fb = 0;
    };
    std::vector<float> pool_;
    std::array<std::array<Delay, 8>, 2> comb_{};
    std::array<std::array<Delay, 4>, 2> allpass_{};
    float line_read(int ch, double delay) const;
};

// 速さを変えずに音程を変える（遅延線を 2 つの読み位置で読み、交互にクロスフェードする）
class PitchShifter {
public:
    void prepare(double fs);
    void set_ratio(double r) { ratio_ = r; }
    double ratio() const { return ratio_; }
    // 掛けている間に増える遅れ（フレーム。平均）
    int latency_frames() const { return ratio_ != 1.0 ? window_ / 2 : 0; }
    void process(float* lr, uint32_t n);

private:
    double ratio_ = 1.0;
    std::vector<float> buf_;
    size_t mask_ = 0, pos_ = 0;
    int window_ = 1920;
    double phase_ = 0;  // 1 つ目の読み位置の遅れ（0〜window_）
};

// 1 つの音（ステレオの pcm）を step 倍の速さで読み、gain[i] * scale 倍して out に足す。pos は読み位置（フレーム。
// 負ならその分だけ遅れて始まる）で、n フレーム分進める。鳴り終わったら false（loop なら頭に戻る）
bool mix_resampled(const float* pcm, int64_t frames, double& pos, double step, bool loop, const float* gain, float scale,
                   float* out, uint32_t n);

// 曲の時計：wall の時点で譜面の時刻が base_us だったとき、dt_us 後にスピーカーから出ている位置。
// rate は再生の速さ（FREQ・SPEED）、latency_us は出力の遅れ（実時間）
int64_t song_time_at(int64_t base_us, int64_t dt_us, int64_t latency_us, double rate);

}  // namespace bmj
