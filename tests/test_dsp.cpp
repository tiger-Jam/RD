#include <cmath>
#include <vector>

#include "audio/dsp.h"
#include "test.h"

using namespace bmj;

namespace {
constexpr double kFs = 48000;
constexpr double kPi = 3.14159265358979323846;
// 左右同じ正弦波（振幅 0.5）
std::vector<float> sine(double hz, int frames) {
    std::vector<float> v(frames * 2);
    for (int i = 0; i < frames; ++i) v[i * 2] = v[i * 2 + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / kFs));
    return v;
}
// 後半（落ち着いたところ）の左の実効値
double rms_tail(const std::vector<float>& v) {
    size_t frames = v.size() / 2, from = frames / 2;
    double sum = 0;
    for (size_t i = from; i < frames; ++i) sum += v[i * 2] * v[i * 2];
    return std::sqrt(sum / (frames - from));
}
// 後半の左が 0 を下から上に横切る回数から周波数を出す
double freq_tail(const std::vector<float>& v) {
    size_t frames = v.size() / 2, from = frames / 2;
    int cross = 0;
    for (size_t i = from + 1; i < frames; ++i)
        if (v[(i - 1) * 2] < 0 && v[i * 2] >= 0) ++cross;
    return cross * kFs / (frames - from);
}
std::vector<float> run(Effector& e, std::vector<float> v) {
    for (size_t off = 0; off < v.size() / 2; off += 512)
        e.process(v.data() + off * 2, static_cast<uint32_t>(std::min<size_t>(512, v.size() / 2 - off)));
    return v;
}
}  // namespace

TEST(dsp_eq_flat_passes_unchanged) {
    // 0 dB のピーキングは素通し。全バンド 0 ならイコライザは何もしない
    std::vector<float> in = sine(997, 4800), out = in;
    Biquad b;
    b.peaking(kFs, 1000, 1.5, 0);
    b.process(out.data(), 4800);
    double maxd = 0;
    for (size_t i = 0; i < in.size(); ++i) maxd = std::max(maxd, static_cast<double>(std::fabs(in[i] - out[i])));
    CHECK(maxd < 1e-5);
    Equalizer eq;
    eq.set(kFs, {});
    CHECK(eq.flat());
    out = in;
    eq.process(out.data(), 4800);
    CHECK(out == in);
}

TEST(dsp_eq_band_boost) {
    // 1kHz を +12 dB：1kHz はおよそ 4 倍、63Hz はほとんど変わらない
    Equalizer eq;
    eq.set(kFs, {0, 0, 0, 12, 0, 0, 0});
    std::vector<float> a = sine(1000, 9600), b = sine(63, 48000);
    double ra = rms_tail(a), rb = rms_tail(b);
    eq.process(a.data(), 9600);
    Equalizer eq2;
    eq2.set(kFs, {0, 0, 0, 12, 0, 0, 0});
    eq2.process(b.data(), 48000);
    CHECK(std::fabs(rms_tail(a) / ra - 3.98) < 0.2);
    CHECK(std::fabs(rms_tail(b) / rb - 1.0) < 0.05);
}

TEST(dsp_lpf_attenuates_high) {
    // LPF の p1=50 は 22000 / 1.06^50 ≒ 1.2kHz。10kHz は 1/10 より小さく、100Hz はほぼそのまま
    Effector e;
    e.prepare(kFs);
    FxSlot s{true, kFxLowPass, kFxMaster, 50, 0};
    e.set(s);
    std::vector<float> hi = sine(10000, 9600), lo = sine(100, 48000);
    double rh = rms_tail(hi), rl = rms_tail(lo);
    CHECK(rms_tail(run(e, hi)) < rh * 0.1);
    e.set(FxSlot{true, kFxHighPass, kFxMaster, 0, 0});
    e.set(s);
    CHECK(std::fabs(rms_tail(run(e, lo)) / rl - 1.0) < 0.05);
}

TEST(dsp_lpf_open_and_hpf) {
    // LPF の p1=100（種類を選んだときの値）は素通し。HPF の p1=70 は 22 * 1.06^70 ≒ 1.3kHz で 100Hz を落とす
    SoundFx fx;
    fx.set_fx_type(0, kFxLowPass);
    CHECK_EQ(fx.fx[0].p1, 100);
    CHECK_EQ(fx.fx[0].p2, 0);
    fx.set_fx_type(0, kFxEcho);
    CHECK_EQ(fx.fx[0].p1, 0);
    Effector e;
    e.prepare(kFs);
    e.set(FxSlot{true, kFxLowPass, kFxMaster, 100, 0});
    std::vector<float> in = sine(15000, 4800);
    CHECK(run(e, in) == in);
    Effector h;
    h.prepare(kFs);
    h.set(FxSlot{true, kFxHighPass, kFxMaster, 70, 0});
    std::vector<float> lo = sine(100, 48000);
    CHECK(rms_tail(run(h, lo)) < rms_tail(lo) * 0.05);
}

TEST(dsp_echo_and_distortion) {
    // エコー p1=10 は 100ms 後に返る（p2=100 で 0 dB）
    Effector e;
    e.prepare(kFs);
    e.set(FxSlot{true, kFxEcho, kFxMaster, 10, 100});
    std::vector<float> v(9600 * 2, 0.0f);
    v[0] = v[1] = 1.0f;
    v = run(e, v);
    CHECK(std::fabs(v[4800 * 2] - 1.0f) < 1e-3);
    CHECK(std::fabs(v[2400 * 2]) < 1e-6);
    // ディストーションの強さ 0 は素通し、強いほど小さい音が持ち上がる
    Effector d;
    d.prepare(kFs);
    d.set(FxSlot{true, kFxDistortion, kFxMaster, 0, 0});
    std::vector<float> in = sine(440, 4800);
    CHECK(run(d, in) == in);
    d.set(FxSlot{true, kFxDistortion, kFxMaster, 80, 0});
    CHECK(rms_tail(run(d, in)) > rms_tail(in) * 1.3);
    // オフなら何もしない
    Effector off;
    off.prepare(kFs);
    off.set(FxSlot{false, kFxReverb, kFxMaster, 50, 100});
    CHECK(run(off, in) == in);
}

TEST(dsp_pitch_rates) {
    SoundFx fx;
    fx.pitch_on = true;
    fx.pitch = 12;
    fx.pitch_type = kPitchFreq;
    CHECK(std::fabs(fx.play_rate() - 2.0) < 1e-12);
    CHECK(fx.shift_ratio() == 1.0);
    CHECK(fx.changes_speed());
    fx.pitch_type = kPitchPitch;  // 音程だけ：速さは変わらない
    CHECK(fx.play_rate() == 1.0);
    CHECK(std::fabs(fx.shift_ratio() - 2.0) < 1e-12);
    CHECK(!fx.changes_speed());
    fx.pitch_type = kPitchSpeed;  // 速さだけ：回転数を上げ、上がった音程を戻す
    CHECK(std::fabs(fx.play_rate() - 2.0) < 1e-12);
    CHECK(std::fabs(fx.shift_ratio() - 0.5) < 1e-12);
    fx.pitch_on = false;  // オフなら 0 と同じ
    CHECK(!fx.changes_speed());
    fx.pitch_on = true;
    fx.pitch = 0;
    CHECK(!fx.changes_speed());
}

TEST(dsp_freq_doubles_song_clock) {
    // FREQ +12：曲の時計は実時間の 2 倍で進み、出力の遅れも譜面の時刻では 2 倍になる
    CHECK_EQ(song_time_at(0, 1000000, 0, 2.0), static_cast<int64_t>(2000000));
    CHECK_EQ(song_time_at(500, 1000000, 10000, 2.0), static_cast<int64_t>(500 + 1980000));
    CHECK_EQ(song_time_at(0, 1000000, 10000, 1.0), static_cast<int64_t>(990000));
    // 音も 2 倍の速さで読む：10 フレームで 20 フレーム進み、1 つおきの値が出る
    std::vector<float> pcm(64 * 2);
    for (int i = 0; i < 64; ++i) pcm[i * 2] = pcm[i * 2 + 1] = static_cast<float>(i);
    std::vector<float> gain(10, 1.0f), out(20, 0.0f);
    double pos = 0;
    CHECK(mix_resampled(pcm.data(), 64, pos, 2.0, false, gain.data(), 1.0f, out.data(), 10));
    CHECK(pos == 20.0);
    CHECK(out[2 * 3] == 6.0f);
    // 負の位置は遅れて始まる。終わりまで行ったら false
    std::fill(out.begin(), out.end(), 0.0f);
    pos = -4;
    CHECK(mix_resampled(pcm.data(), 64, pos, 2.0, false, gain.data(), 1.0f, out.data(), 10));
    CHECK(out[0] == 0.0f && out[2 * 2] == 0.0f && out[2 * 3] == 2.0f);
    pos = 60;
    CHECK(!mix_resampled(pcm.data(), 64, pos, 2.0, false, gain.data(), 1.0f, out.data(), 10));
}

TEST(dsp_pitch_shifter_changes_pitch_only) {
    // 440Hz を 2 倍の音程に。長さ（フレーム数）はそのまま
    PitchShifter p;
    p.prepare(kFs);
    p.set_ratio(2.0);
    CHECK(p.latency_frames() > 0);
    std::vector<float> v = sine(440, 48000);
    for (size_t off = 0; off < 48000; off += 480) p.process(v.data() + off * 2, 480);
    CHECK(std::fabs(freq_tail(v) - 880) < 40);
    PitchShifter same;
    same.prepare(kFs);
    std::vector<float> in = sine(440, 4800), out = in;
    same.process(out.data(), 4800);
    CHECK(out == in);
    CHECK_EQ(same.latency_frames(), 0);
}
