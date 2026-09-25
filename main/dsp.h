// Small fixed-allocation float DSP blocks. No framework dependencies, so this
// file compiles anywhere; everything is sized at build time and lives in .bss.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace dsp {

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// Pade-ish tanh: ~3 % error, no libm call, ~15 cycles.
inline float softClip(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// --------------------------------------------------------------------------
class DCBlocker {
public:
    inline float process(float x) {
        const float y = x - x1_ + 0.9975f * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
    void reset() { x1_ = y1_ = 0.0f; }

private:
    float x1_ = 0.0f, y1_ = 0.0f;
};

// --------------------------------------------------------------------------
class OnePoleLP {
public:
    void setCutoff(float hz, float fs) { a_ = 1.0f - expf(-2.0f * kPi * hz / fs); }
    inline float process(float x) {
        z_ += a_ * (x - z_);
        return z_;
    }
    void reset() { z_ = 0.0f; }

private:
    float a_ = 0.2f, z_ = 0.0f;
};

// ------------------------------------------------- RBJ biquad, TDF-II ----
class Biquad {
public:
    void lowpass(float fc, float q, float fs);
    void highpass(float fc, float q, float fs);
    void peaking(float fc, float q, float gainDb, float fs);

    inline float process(float x) {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    void reset() { z1_ = z2_ = 0.0f; }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

// ------------------------------------------------------------- gate -------
// Kills the hiss and, more importantly, the feedback loop between the speaker
// on the outside of the mask and the mic on the inside while nobody is
// talking. Hysteresis stops it chattering on breath noise.
class NoiseGate {
public:
    void init(float fs);
    void setThreshold(float linear) { thr_ = linear; }

    inline float process(float x) {
        const float a = fabsf(x);
        env_ = (a > env_) ? a + envAtk_ * (env_ - a) : a + envRel_ * (env_ - a);
        open_ = open_ ? (env_ > thr_ * 0.5f) : (env_ > thr_);
        const float target = open_ ? 1.0f : 0.0f;
        gain_ += (target - gain_) * (target > gain_ ? gAtk_ : gRel_);
        return x * gain_;
    }
    float envelope() const { return env_; }
    void reset() { env_ = gain_ = 0.0f; open_ = false; }

private:
    float env_ = 0.0f, gain_ = 0.0f, thr_ = 0.004f;
    float envAtk_ = 0.0f, envRel_ = 0.0f, gAtk_ = 0.0f, gRel_ = 0.0f;
    bool open_ = false;
};

// --------------------------------------------------- steep lowpass -------
// Sixth-order Butterworth, three cascaded biquads. Used to band-limit the
// input ahead of an upward pitch shift: a single 2nd-order section only
// manages ~10 dB at 1.4x its cutoff, which is not enough to stop sibilants
// folding back over Nyquist and turning metallic.
class SteepLowpass {
public:
    void set(float fc, float fs) {
        // Butterworth pole Qs for a 6th-order cascade.
        static const float kQ[3] = {0.5176f, 0.7071f, 1.9319f};
        for (int i = 0; i < 3; ++i) stage_[i].lowpass(fc, kQ[i], fs);
    }
    inline float process(float x) {
        return stage_[2].process(stage_[1].process(stage_[0].process(x)));
    }
    void reset() {
        for (int i = 0; i < 3; ++i) stage_[i].reset();
    }

private:
    Biquad stage_[3];
};

// ------------------------------------------------------- pitch shifter ----
// WSOLA (waveform-similarity overlap-add) on a delay line.
//
// A single read pointer runs at `ratio` times write speed, so the delay ramps
// and the pitch scales. After a grain the pointer has to jump back, and the
// naive thing - jumping a fixed distance and crossfading two taps - is what
// makes cheap shifters warble: a fixed jump lands at an arbitrary point in
// the waveform, so the splice flips or smears the phase at the grain rate.
// Measured on a 220 Hz tone shifted down 7 semitones, that suppressed the
// wanted 146.8 Hz outright (-28 dB) and left only sidebands 10 Hz either
// side, which is amplitude modulation, not pitch shifting.
//
// So at each splice we search +/-kSearch samples for the jump that best
// matches the waveform we have been reading, then crossfade over kFadeLen.
// The search runs once per grain (~10-25 times a second), not per sample.
class PitchShifter {
public:
    static constexpr int kLen = 2048;      // 64 ms @ 32 kHz, power of two
    static constexpr int kMask = kLen - 1;
    static constexpr int kGrain = 1024;    // nominal grain, 32 ms
    static constexpr int kFadeLen = 160;   // splice crossfade, 5 ms
    static constexpr int kSearch = 288;    // +/-9 ms: covers F0 down to ~55 Hz
    static constexpr int kCorrLen = 128;   // similarity window, 4 ms
    static constexpr int kCorrStep = 3;    // coarse search stride

    void setRatio(float ratio);            // 0.5 = one octave down
    void reset();
    float process(float x);

private:
    inline float read(float delay) const {
        const float idx = static_cast<float>(w_) - delay;
        const int i0 = static_cast<int>(floorf(idx));
        const float frac = idx - static_cast<float>(i0);
        const float a = buf_[i0 & kMask];
        const float b = buf_[(i0 + 1) & kMask];
        return a + frac * (b - a);
    }

    float similarity(int ia, float dCandidate) const;
    int findLag(float dBase) const;
    void startSplice(float dBase);

    float buf_[kLen] = {};
    int w_ = 0;
    float dA_ = 0.0f;      // live tap delay
    float dB_ = 0.0f;      // incoming tap delay, valid while splicing
    float dInc_ = 0.0f;    // delay growth per sample = 1 - ratio
    int fade_ = 0;         // samples left in the crossfade, 0 = none
};

// ------------------------------------------------------------ ring mod ----
// Table-driven sine. At 20-40 Hz it adds the "throat rattle" growl; up around
// 60-100 Hz it turns metallic/possessed.
class RingMod {
public:
    static void buildTable();
    void setFreq(float hz, float fs) { inc_ = hz / fs; }
    inline float next() {
        const float p = phase_ * kTableSize;
        const int i = static_cast<int>(p);
        const float frac = p - static_cast<float>(i);
        const float a = table_[i & (kTableSize - 1)];
        const float b = table_[(i + 1) & (kTableSize - 1)];
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        return a + frac * (b - a);
    }
    void reset() { phase_ = 0.0f; }

private:
    static constexpr int kTableSize = 256;
    static float table_[kTableSize];
    float phase_ = 0.0f, inc_ = 0.0f;
};

// ------------------------------------------------------------- reverb -----
// Three damped Freeverb-style combs into two allpasses. Small, mono, and just
// enough to make the voice sound like it is coming out of a crypt.
class Reverb {
public:
    void init(float fs);
    void setDecay(float fb) { fb_ = clampf(fb, 0.0f, 0.92f); }
    float process(float x);   // wet only
    void reset();

private:
    static constexpr int kC1 = 1237, kC2 = 1601, kC3 = 1811;
    static constexpr int kA1 = 331, kA2 = 113;

    float c1_[kC1] = {}, c2_[kC2] = {}, c3_[kC3] = {};
    float a1_[kA1] = {}, a2_[kA2] = {};
    int i1_ = 0, i2_ = 0, i3_ = 0, j1_ = 0, j2_ = 0;
    float d1_ = 0.0f, d2_ = 0.0f, d3_ = 0.0f;   // comb damping states
    float fb_ = 0.78f, damp_ = 0.4f;
};

// ------------------------------------------------------------ limiter -----
// Last line of defence: the amp clips hard and ugly, and a clipped 3 W burst
// inside a mask is unpleasant. Fast attack, slow release, soft clip on top.
class Limiter {
public:
    void init(float fs);
    void setCeiling(float c) { ceiling_ = clampf(c, 0.1f, 0.99f); }
    inline float process(float x) {
        const float a = fabsf(x);
        env_ = (a > env_) ? a + atk_ * (env_ - a) : a + rel_ * (env_ - a);
        const float g = (env_ > ceiling_) ? ceiling_ / env_ : 1.0f;
        gain_ += (g - gain_) * (g < gain_ ? 0.4f : 0.0008f);
        return softClip(x * gain_);
    }
    void reset() { env_ = 0.0f; gain_ = 1.0f; }

private:
    float ceiling_ = 0.85f;
    float env_ = 0.0f, gain_ = 1.0f, atk_ = 0.0f, rel_ = 0.0f;
};

}  // namespace dsp
