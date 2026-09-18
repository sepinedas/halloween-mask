#include "dsp.h"

#include <string.h>

namespace dsp {

// ---------------------------------------------------------------- Biquad --
void Biquad::lowpass(float fc, float q, float fs) {
    const float w0 = 2.0f * kPi * fc / fs;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0_ = ((1.0f - c) * 0.5f) / a0;
    b1_ = (1.0f - c) / a0;
    b2_ = b0_;
    a1_ = (-2.0f * c) / a0;
    a2_ = (1.0f - alpha) / a0;
}

void Biquad::highpass(float fc, float q, float fs) {
    const float w0 = 2.0f * kPi * fc / fs;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0_ = ((1.0f + c) * 0.5f) / a0;
    b1_ = -(1.0f + c) / a0;
    b2_ = b0_;
    a1_ = (-2.0f * c) / a0;
    a2_ = (1.0f - alpha) / a0;
}

void Biquad::peaking(float fc, float q, float gainDb, float fs) {
    const float A = powf(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * fc / fs;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha / A;
    b0_ = (1.0f + alpha * A) / a0;
    b1_ = (-2.0f * c) / a0;
    b2_ = (1.0f - alpha * A) / a0;
    a1_ = b1_;
    a2_ = (1.0f - alpha / A) / a0;
}

// ------------------------------------------------------------- NoiseGate --
void NoiseGate::init(float fs) {
    envAtk_ = expf(-1.0f / (0.002f * fs));   // 2 ms
    envRel_ = expf(-1.0f / (0.080f * fs));   // 80 ms
    gAtk_ = 1.0f - expf(-1.0f / (0.003f * fs));
    gRel_ = 1.0f - expf(-1.0f / (0.120f * fs));
    reset();
}

// ---------------------------------------------------------- PitchShifter --
void PitchShifter::setRatio(float ratio) {
    ratio = clampf(ratio, 0.25f, 2.0f);
    // The read pointer must advance at `ratio`, so the delay grows by
    // (1 - ratio) every sample. Everything else follows from that.
    dInc_ = 1.0f - ratio;
}

void PitchShifter::reset() {
    memset(buf_, 0, sizeof(buf_));
    w_ = 0;
    dA_ = (dInc_ >= 0.0f) ? 0.0f : static_cast<float>(kGrain);
    dB_ = dA_;
    fade_ = 0;
}

// Normalised cross-correlation between the kCorrLen samples just read by the
// live tap and the ones a candidate tap would read next.
float PitchShifter::similarity(int ia, float dCandidate) const {
    const int ib = (w_ - static_cast<int>(dCandidate)) & kMask;
    float num = 0.0f, den = 1e-9f;
    for (int k = 0; k < kCorrLen; ++k) {
        const float a = buf_[(ia - k) & kMask];
        const float b = buf_[(ib - k) & kMask];
        num += a * b;
        den += b * b;
    }
    // Normalising by the candidate energy only: we want the best-matching
    // shape, without preferring whichever window happens to be loudest.
    return num / sqrtf(den);
}

int PitchShifter::findLag(float dBase) const {
    const int ia = (w_ - static_cast<int>(dA_)) & kMask;

    int bestLag = 0;
    float best = -1e30f;
    for (int lag = -kSearch; lag <= kSearch; lag += kCorrStep) {
        const float s = similarity(ia, dBase + static_cast<float>(lag));
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    // The coarse stride can land a sample or two off the true peak, which is
    // exactly the error that smears the splice. Refine.
    for (int lag = bestLag - kCorrStep + 1; lag < bestLag + kCorrStep; ++lag) {
        if (lag == bestLag || lag < -kSearch || lag > kSearch) continue;
        const float s = similarity(ia, dBase + static_cast<float>(lag));
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    return bestLag;
}

void PitchShifter::startSplice(float dBase) {
    dB_ = dBase + static_cast<float>(findLag(dBase));
    fade_ = kFadeLen;
}

float PitchShifter::process(float x) {
    buf_[w_] = x;

    float y;
    if (fade_ > 0) {
        const float t = 1.0f - static_cast<float>(fade_) / kFadeLen;
        const float g = 0.5f - 0.5f * cosf(kPi * t);     // raised-cosine splice
        y = (1.0f - g) * read(dA_) + g * read(dB_);
    } else {
        y = read(dA_);
    }

    w_ = (w_ + 1) & kMask;
    dA_ += dInc_;

    if (fade_ > 0) {
        dB_ += dInc_;
        if (--fade_ == 0) dA_ = dB_;      // incoming tap takes over
    } else if (dInc_ > 0.0f) {
        // Pitching down: the delay grows, jump back by about a grain.
        if (dA_ >= static_cast<float>(kGrain)) {
            startSplice(dA_ - kGrain + kSearch);
        }
    } else if (dInc_ < 0.0f) {
        // Pitching up: the delay shrinks, jump forward before it hits zero.
        if (dA_ <= static_cast<float>(kFadeLen + 8)) {
            startSplice(dA_ + kGrain - kSearch);
        }
    }
    return y;
}

// --------------------------------------------------------------- RingMod --
float RingMod::table_[RingMod::kTableSize] = {};

void RingMod::buildTable() {
    for (int i = 0; i < kTableSize; ++i) {
        table_[i] = sinf(2.0f * kPi * static_cast<float>(i) / kTableSize);
    }
}

// ---------------------------------------------------------------- Reverb --
void Reverb::init(float fs) {
    // Comb delays are tuned for 32 kHz; scale them if you change the rate.
    (void)fs;
    reset();
}

void Reverb::reset() {
    memset(c1_, 0, sizeof(c1_));
    memset(c2_, 0, sizeof(c2_));
    memset(c3_, 0, sizeof(c3_));
    memset(a1_, 0, sizeof(a1_));
    memset(a2_, 0, sizeof(a2_));
    i1_ = i2_ = i3_ = j1_ = j2_ = 0;
    d1_ = d2_ = d3_ = 0.0f;
}

float Reverb::process(float x) {
    float y = 0.0f;

    float s = c1_[i1_];
    d1_ = s * (1.0f - damp_) + d1_ * damp_;
    c1_[i1_] = x + d1_ * fb_;
    if (++i1_ >= kC1) i1_ = 0;
    y += s;

    s = c2_[i2_];
    d2_ = s * (1.0f - damp_) + d2_ * damp_;
    c2_[i2_] = x + d2_ * fb_;
    if (++i2_ >= kC2) i2_ = 0;
    y += s;

    s = c3_[i3_];
    d3_ = s * (1.0f - damp_) + d3_ * damp_;
    c3_[i3_] = x + d3_ * fb_;
    if (++i3_ >= kC3) i3_ = 0;
    y += s;

    y *= 0.333f;

    float b = a1_[j1_];
    a1_[j1_] = y + b * 0.5f;
    y = b - y;
    if (++j1_ >= kA1) j1_ = 0;

    b = a2_[j2_];
    a2_[j2_] = y + b * 0.5f;
    y = b - y;
    if (++j2_ >= kA2) j2_ = 0;

    return y;
}

// --------------------------------------------------------------- Limiter --
void Limiter::init(float fs) {
    atk_ = expf(-1.0f / (0.0007f * fs));   // 0.7 ms
    rel_ = expf(-1.0f / (0.200f * fs));    // 200 ms
    reset();
}

}  // namespace dsp
