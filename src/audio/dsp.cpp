#include "audio/dsp.h"

#include <algorithm>
#include <cmath>

namespace bmj {

namespace {
constexpr double kPi = 3.14159265358979323846;
// LR2 のパラメータ（0〜100）を割合とみなして dB にする（0 なら下限）
double percent_db(int p, double lo, double hi) {
    if (p <= 0) return lo;
    return std::clamp(10.0 * std::log10(p / 100.0), lo, hi);
}
double db_gain(double db) { return std::pow(10.0, db / 20.0); }
size_t pow2_at_least(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
}  // namespace

double semitone_ratio(int n) { return std::pow(2.0, n / 12.0); }

double SoundFx::play_rate() const {
    if (!pitch_on || pitch == 0 || pitch_type == kPitchPitch) return 1.0;
    return semitone_ratio(pitch);
}

double SoundFx::shift_ratio() const {
    if (!pitch_on || pitch == 0) return 1.0;
    if (pitch_type == kPitchPitch) return semitone_ratio(pitch);
    if (pitch_type == kPitchSpeed) return semitone_ratio(-pitch);
    return 1.0;
}

void SoundFx::set_fx_type(int slot, int type) {
    if (slot < 0 || slot >= 3) return;
    FxSlot& s = fx[slot];
    s.type = std::clamp(type, 0, kFxTypeCount - 1);
    s.p1 = s.type == kFxLowPass ? 100 : 0;
    s.p2 = 0;
}

void SoundFx::clamp() {
    for (int& g : eq) g = std::clamp(g, -12, 12);
    pitch = std::clamp(pitch, -12, 12);
    pitch_type = std::clamp(pitch_type, 0, 2);
    for (FxSlot& s : fx) {
        s.type = std::clamp(s.type, 0, kFxTypeCount - 1);
        s.target = std::clamp(s.target, 0, 2);
        s.p1 = std::clamp(s.p1, 0, 100);
        s.p2 = std::clamp(s.p2, 0, 100);
    }
}

// ---- 2 次の IIR（係数は RBJ の Audio EQ Cookbook の式） ----

void Biquad::peaking(double fs, double f, double q, double db) {
    double a = std::pow(10.0, db / 40.0);
    double w = 2 * kPi * std::min(f, fs * 0.45) / fs;
    double alpha = std::sin(w) / (2 * q);
    double c = std::cos(w);
    double a0 = 1 + alpha / a;
    b0 = (1 + alpha * a) / a0;
    b1 = -2 * c / a0;
    b2 = (1 - alpha * a) / a0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha / a) / a0;
}

void Biquad::lowpass(double fs, double f, double q) {
    double w = 2 * kPi * std::min(f, fs * 0.45) / fs;
    double alpha = std::sin(w) / (2 * q);
    double c = std::cos(w);
    double a0 = 1 + alpha;
    b0 = (1 - c) / 2 / a0;
    b1 = (1 - c) / a0;
    b2 = b0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
}

void Biquad::highpass(double fs, double f, double q) {
    double w = 2 * kPi * std::min(f, fs * 0.45) / fs;
    double alpha = std::sin(w) / (2 * q);
    double c = std::cos(w);
    double a0 = 1 + alpha;
    b0 = (1 + c) / 2 / a0;
    b1 = -(1 + c) / a0;
    b2 = b0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
}

void Biquad::process(float* lr, uint32_t n) {
    for (int ch = 0; ch < 2; ++ch) {
        double s1 = z[ch][0], s2 = z[ch][1];
        for (uint32_t i = 0; i < n; ++i) {
            double x = lr[i * 2 + ch];
            double y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            lr[i * 2 + ch] = static_cast<float>(y);
        }
        z[ch][0] = s1, z[ch][1] = s2;
    }
}

// ---- イコライザ ----

void Equalizer::set(double fs, const std::array<int, 7>& gains) {
    flat_ = true;
    for (int i = 0; i < 7; ++i) {
        bool on = gains[i] != 0;
        if (on) band_[i].peaking(fs, kFreq[i], 1.5, gains[i]);
        // 途中から入れたバンドは、前の音の状態を持ち越さない
        if (on && !on_[i]) band_[i].reset();
        on_[i] = on;
        flat_ = flat_ && !on;
    }
}

void Equalizer::process(float* lr, uint32_t n) {
    if (flat_) return;
    for (int i = 0; i < 7; ++i)
        if (on_[i]) band_[i].process(lr, n);
}

// ---- エフェクタ ----

void Effector::prepare(double fs) {
    fs_ = fs;
    // エコーは最長 5 秒。フランジャー・コーラスも同じ線を使う
    line_.assign(pow2_at_least(static_cast<size_t>(fs * 5.0) + 4) * 2, 0.0f);
    line_mask_ = line_.size() / 2 - 1;
    // リバーブの遅延の長さ（44.1kHz でのサンプル数。右は少しずらして広がりを出す）
    static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAllpass[4] = {556, 441, 341, 225};
    size_t total = 0;
    auto place = [&](Delay& d, int len44) {
        d.len = std::max<size_t>(1, static_cast<size_t>(len44 * fs / 44100.0));
        d.off = total;
        total += d.len;
    };
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 8; ++i) place(comb_[ch][i], kComb[i] + ch * 23);
        for (int i = 0; i < 4; ++i) place(allpass_[ch][i], kAllpass[i] + ch * 23);
    }
    pool_.assign(total, 0.0f);
    reset();
}

void Effector::reset() {
    std::fill(line_.begin(), line_.end(), 0.0f);
    std::fill(pool_.begin(), pool_.end(), 0.0f);
    for (auto& side : comb_)
        for (Delay& d : side) d.pos = 0, d.store = 0;
    for (auto& side : allpass_)
        for (Delay& d : side) d.pos = 0;
    line_pos_ = 0;
    lfo_ = 0;
    filter_.reset();
}

void Effector::set(const FxSlot& s) {
    bool active = s.on && s.type > kFxOff && s.type < kFxTypeCount;
    if (s.type != type_ || (active && !active_)) reset();
    type_ = s.type;
    target_ = s.target;
    active_ = active;
    // パラメータの換算は LR2 のもの（スライダーの値 → 各効果の値）
    switch (type_) {
    case kFxReverb: {
        a_ = std::clamp(s.p1 * 200.0, 100.0, 20000.0) / 1000.0;  // 残響の長さ（秒）
        b_ = db_gain(percent_db(s.p2, -80, 20));                  // 残響の音量
        for (auto& side : comb_)
            for (Delay& d : side)
                d.fb = static_cast<float>(std::pow(10.0, -3.0 * static_cast<double>(d.len) / (a_ * fs_)));
        break;
    }
    case kFxEcho:
        a_ = std::clamp(s.p1 * 10.0, 1.0, 5000.0) / 1000.0 * fs_;  // 遅れ（サンプル）
        b_ = db_gain(percent_db(s.p2, -80, 10));                   // 返りの音量
        break;
    case kFxLowPass: {
        double f = std::clamp(22000.0 / std::pow(1.06, 100 - s.p1), 1.0, 22000.0);
        double q = std::clamp(s.p2 * 9.0 / 100.0 + 1.0, 0.1, 10.0);
        a_ = f;
        if (f >= fs_ * 0.45) filter_.identity();  // 出せる一番上より上なら、開いたまま
        else filter_.lowpass(fs_, f, q);
        break;
    }
    case kFxHighPass: {
        double f = std::clamp(22.0 * std::pow(1.06, s.p1), 1.0, 22000.0);
        double q = std::clamp(s.p2 * 9.0 / 100.0 + 1.0, 0.1, 10.0);
        a_ = f;
        filter_.highpass(fs_, f, q);
        break;
    }
    case kFxFlanger:
        a_ = std::clamp(s.p1 / 100.0, 0.01, 1.0);                   // 深さ
        b_ = std::clamp(0.02 * std::pow(1.06, s.p2), 0.0, 20.0);   // 揺れの速さ（Hz）
        break;
    case kFxChorus:
        a_ = std::clamp(s.p1 / 5.0, 0.0, 20.0);   // 揺れの速さ（Hz）
        b_ = std::clamp(s.p2 / 1.0, 0.0, 100.0);  // 深さ（%）
        break;
    case kFxDistortion:
        a_ = std::clamp(s.p1 / 100.0, 0.0, 1.0);  // 歪みの強さ
        break;
    default: break;
    }
}

float Effector::line_read(int ch, double delay) const {
    double r = static_cast<double>(line_pos_) - delay;
    double fl = std::floor(r);
    float f = static_cast<float>(r - fl);
    size_t i0 = static_cast<size_t>(static_cast<int64_t>(fl)) & line_mask_;
    size_t i1 = (i0 + 1) & line_mask_;
    float s0 = line_[i0 * 2 + ch], s1 = line_[i1 * 2 + ch];
    return s0 + (s1 - s0) * f;
}

void Effector::process(float* lr, uint32_t n) {
    if (!active_) return;
    switch (type_) {
    case kFxReverb: {
        const float wet = static_cast<float>(b_ * 3.0);
        for (uint32_t i = 0; i < n; ++i) {
            float in = (lr[i * 2] + lr[i * 2 + 1]) * 0.015f;
            for (int ch = 0; ch < 2; ++ch) {
                float acc = 0;
                for (Delay& d : comb_[ch]) {
                    float* buf = &pool_[d.off];
                    float y = buf[d.pos];
                    d.store = y * 0.8f + d.store * 0.2f;  // 高い音ほど早く消える
                    buf[d.pos] = in + d.store * d.fb;
                    if (++d.pos >= d.len) d.pos = 0;
                    acc += y;
                }
                for (Delay& d : allpass_[ch]) {
                    float* buf = &pool_[d.off];
                    float b = buf[d.pos];
                    buf[d.pos] = acc + b * 0.5f;
                    acc = b - acc;
                    if (++d.pos >= d.len) d.pos = 0;
                }
                lr[i * 2 + ch] += acc * wet;
            }
        }
        break;
    }
    case kFxEcho: {
        const float wet = static_cast<float>(b_);
        for (uint32_t i = 0; i < n; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                float d = line_read(ch, a_);
                float x = lr[i * 2 + ch];
                line_[line_pos_ * 2 + ch] = x + d * 0.5f;  // 返りの半分をまた返す
                lr[i * 2 + ch] = x + d * wet;
            }
            line_pos_ = (line_pos_ + 1) & line_mask_;
        }
        break;
    }
    case kFxLowPass:
    case kFxHighPass: filter_.process(lr, n); break;
    case kFxFlanger: {
        // 0.1〜5.1ms の短い遅れを揺らして、元の音と半々に混ぜる
        const double step = b_ / fs_;
        for (uint32_t i = 0; i < n; ++i) {
            double m = 0.5 - 0.5 * std::cos(2 * kPi * lfo_);
            double delay = (0.0001 + a_ * 0.005 * m) * fs_ + 1.0;
            for (int ch = 0; ch < 2; ++ch) {
                float x = lr[i * 2 + ch];
                line_[line_pos_ * 2 + ch] = x;
                lr[i * 2 + ch] = 0.5f * x + 0.5f * line_read(ch, delay);
            }
            line_pos_ = (line_pos_ + 1) & line_mask_;
            lfo_ += step;
            if (lfo_ >= 1) lfo_ -= 1;
        }
        break;
    }
    case kFxChorus: {
        // 20ms を中心に揺らした 3 つの遅れ（位相を 1/3 ずつずらす）を、元の音と半々に混ぜる
        const double step = a_ / fs_;
        const double swing = b_ / 100.0 * 0.015 * fs_;
        const double center = 0.020 * fs_;
        for (uint32_t i = 0; i < n; ++i) {
            for (int ch = 0; ch < 2; ++ch) line_[line_pos_ * 2 + ch] = lr[i * 2 + ch];
            for (int ch = 0; ch < 2; ++ch) {
                float acc = 0;
                for (int k = 0; k < 3; ++k) acc += line_read(ch, center + swing * std::sin(2 * kPi * (lfo_ + k / 3.0 + ch * 0.25)));
                lr[i * 2 + ch] = 0.5f * lr[i * 2 + ch] + 0.5f * acc / 3.0f;
            }
            line_pos_ = (line_pos_ + 1) & line_mask_;
            lfo_ += step;
            if (lfo_ >= 1) lfo_ -= 1;
        }
        break;
    }
    case kFxDistortion: {
        if (a_ <= 0) break;
        const float k = static_cast<float>(2 * std::min(a_, 0.999) / (1 - std::min(a_, 0.999)));
        for (uint32_t i = 0; i < n * 2; ++i) lr[i] = (1 + k) * lr[i] / (1 + k * std::fabs(lr[i]));
        break;
    }
    default: break;
    }
}

// ---- 音程だけを変える ----

void PitchShifter::prepare(double fs) {
    window_ = std::max(64, static_cast<int>(fs * 0.04));  // 40ms ごとに読み位置を入れ替える
    buf_.assign(pow2_at_least(static_cast<size_t>(window_) * 2 + 4) * 2, 0.0f);
    mask_ = buf_.size() / 2 - 1;
    pos_ = 0;
    phase_ = 0;
}

void PitchShifter::process(float* lr, uint32_t n) {
    if (buf_.empty()) return;
    const double w = window_;
    const double drift = 1.0 - ratio_;  // 読み位置の遅れが 1 サンプルごとに増える量
    for (uint32_t i = 0; i < n; ++i) {
        buf_[pos_ * 2] = lr[i * 2];
        buf_[pos_ * 2 + 1] = lr[i * 2 + 1];
        if (ratio_ != 1.0) {
            float acc[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                double d = phase_ + k * w * 0.5;
                if (d >= w) d -= w;
                // 遅れが 0 と window_ のところで音量が 0 になる窓（2 つ足すと 1）
                double s = std::sin(kPi * d / w);
                float g = static_cast<float>(s * s);
                double r = static_cast<double>(pos_) - d;
                double fl = std::floor(r);
                float f = static_cast<float>(r - fl);
                size_t i0 = static_cast<size_t>(static_cast<int64_t>(fl)) & mask_;
                size_t i1 = (i0 + 1) & mask_;
                for (int ch = 0; ch < 2; ++ch) {
                    float s0 = buf_[i0 * 2 + ch], s1 = buf_[i1 * 2 + ch];
                    acc[ch] += (s0 + (s1 - s0) * f) * g;
                }
            }
            lr[i * 2] = acc[0];
            lr[i * 2 + 1] = acc[1];
            phase_ += drift;
            if (phase_ >= w) phase_ -= w;
            if (phase_ < 0) phase_ += w;
        }
        pos_ = (pos_ + 1) & mask_;
    }
}

// ---- ミキサーの中の計算 ----

namespace {

template <typename T>
bool mix_resampled_t(const T* pcm, int64_t frames, double& pos, double step, bool loop, const float* gain, float scale,
                     float* out, uint32_t n) {
    if (frames <= 0) return false;
    const double len = static_cast<double>(frames);
    for (uint32_t i = 0; i < n; ++i, pos += step) {
        if (pos < 0) continue;
        if (pos >= len) {
            if (!loop) return false;
            pos = std::fmod(pos, len);
        }
        int64_t p = static_cast<int64_t>(pos);
        float f = static_cast<float>(pos - static_cast<double>(p));
        int64_t q = p + 1 < frames ? p + 1 : (loop ? 0 : p);
        const float lp = static_cast<float>(pcm[p * 2]), lq = static_cast<float>(pcm[q * 2]);
        const float rp = static_cast<float>(pcm[p * 2 + 1]), rq = static_cast<float>(pcm[q * 2 + 1]);
        float l = lp + (lq - lp) * f;
        float r = rp + (rq - rp) * f;
        float g = gain[i] * scale;
        out[i * 2] += l * g;
        out[i * 2 + 1] += r * g;
    }
    return loop || pos < len;
}

}  // namespace

bool mix_resampled(const float* pcm, int64_t frames, double& pos, double step, bool loop, const float* gain, float scale,
                   float* out, uint32_t n) {
    return mix_resampled_t(pcm, frames, pos, step, loop, gain, scale, out, n);
}

bool mix_resampled(const int16_t* pcm, int64_t frames, double& pos, double step, bool loop, const float* gain, float scale,
                   float* out, uint32_t n) {
    return mix_resampled_t(pcm, frames, pos, step, loop, gain, scale / 32767.0f, out, n);
}

int64_t song_time_at(int64_t base_us, int64_t dt_us, int64_t latency_us, double rate) {
    return base_us + static_cast<int64_t>(std::llround(static_cast<double>(dt_us - latency_us) * rate));
}

}  // namespace bmj
