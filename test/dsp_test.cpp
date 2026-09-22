// Host-side tests for the mask DSP. No ESP-IDF needed:
//
//   g++ -std=c++17 -O2 -I../main -o dsp_test dsp_test.cpp ../main/dsp.cpp && ./dsp_test
#include "dsp.h"

#include <cmath>
#include <cstdio>
#include <vector>

static const float FS = 32000.0f;

static float estimateFreq(const std::vector<float>& x, int from) {
    // Goertzel sweep: robust where zero-crossing counting is not, because the
    // two crossfading taps interfere and add spurious crossings.
    const int n = (int)x.size() - from;
    float best = 0.0f, bestMag = -1.0f;
    for (float f = 40.0f; f <= 700.0f; f += 0.25f) {
        const float w = 2.0f * dsp::kPi * f / FS;
        const float c = 2.0f * std::cos(w);
        float s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float s0 = x[from + i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float mag = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (mag > bestMag) { bestMag = mag; best = f; }
    }
    return best;
}

static float magAt(const std::vector<float>& x, int from, float f) {
    const int n = (int)x.size() - from;
    const float w = 2.0f * dsp::kPi * f / FS;
    const float c = 2.0f * std::cos(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float s0 = x[from + i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0f, s1 * s1 + s2 * s2 - c * s1 * s2)) / n;
}

static bool finite(const std::vector<float>& x) {
    for (float v : x) if (!std::isfinite(v)) return false;
    return true;
}

static float peak(const std::vector<float>& x, int from) {
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static void testPitch(float semis, float expectedHz) {
    dsp::PitchShifter ps;
    ps.reset();
    ps.setRatio(std::pow(2.0f, semis / 12.0f));

    const int n = (int)FS;
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        const float in = 0.3f * std::sin(2.0f * dsp::kPi * 220.0f * i / FS);
        out[i] = ps.process(in);
    }
    const float f = estimateFreq(out, n / 2);
    const float p = peak(out, n / 2);
    printf("  %+.0f st -> %.1f Hz (expect %.1f), peak %.3f\n", semis, f, expectedHz, p);
    check(std::fabs(f - expectedHz) < expectedHz * 0.06f, "pitch ratio within 6%");
    check(finite(out), "no NaN/Inf");
    // unity-sum crossfade: amplitude should stay near the 0.3 input
    check(p > 0.20f && p < 0.36f, "amplitude preserved");
}

int main() {
    dsp::RingMod::buildTable();

    printf("pitch shifter\n");
    testPitch(-7.0f, 220.0f * std::pow(2.0f, -7.0f / 12.0f));
    testPitch(-12.0f, 110.0f);
    testPitch(0.0f, 220.0f);
    testPitch(+5.0f, 220.0f * std::pow(2.0f, 5.0f / 12.0f));
    testPitch(+12.0f, 440.0f);   // Squirrel preset sits at the ratio clamp

    printf("grain sidebands (regression: fixed-jump splices suppressed the carrier)\n");
    for (float semis : {-7.0f, -12.0f}) {
        const float f0 = 220.0f;
        const float r = std::pow(2.0f, semis / 12.0f);
        const float carrier = f0 * r;
        const float grain = std::fabs(1.0f - r) / dsp::PitchShifter::kGrain * FS;

        dsp::PitchShifter ps;
        ps.setRatio(r);
        ps.reset();
        std::vector<float> out((int)FS);
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = ps.process(0.3f * std::sin(2.0f * dsp::kPi * f0 * i / FS));
        }
        const float c = magAt(out, 8000, carrier);
        const float worst = std::max(magAt(out, 8000, carrier - grain),
                                     magAt(out, 8000, carrier + grain));
        printf("  %+.0f st: carrier %.4f, worst sideband %.4f (%.1f dB down)\n", semis, c,
               worst, 20.0f * std::log10(c / std::max(worst, 1e-9f)));
        check(c > 0.12f, "carrier survives at near full amplitude");
        check(worst < c * 0.1f, "grain sidebands at least 20 dB below carrier");
    }

    printf("squirrel anti-aliasing (+12 st folds >8 kHz input back into band)\n");
    {
        // A 10 kHz input shifted up an octave lands at 20 kHz, past the 16 kHz
        // Nyquist, and folds back to 12 kHz. The Squirrel preset band-limits
        // the input first; without that, sibilants turn metallic.
        const float fin = 10000.0f, falias = FS - 2.0f * fin;  // 20 kHz folds to 12 kHz
        float aliasNoAA = 0.0f, aliasAA = 0.0f;

        for (int withAA = 0; withAA < 2; ++withAA) {
            dsp::PitchShifter ps;
            ps.setRatio(2.0f);
            ps.reset();
            dsp::SteepLowpass aa;
            aa.set(0.45f * FS / 2.0f, FS);

            std::vector<float> out((int)FS);
            for (size_t i = 0; i < out.size(); ++i) {
                float x = 0.3f * std::sin(2.0f * dsp::kPi * fin * i / FS);
                if (withAA) x = aa.process(x);
                out[i] = ps.process(x);
            }
            (withAA ? aliasAA : aliasNoAA) = magAt(out, 8000, falias);
        }
        printf("  alias at %.0f Hz: %.5f without filter, %.5f with (%.1f dB better)\n",
               falias, aliasNoAA, aliasAA,
               20.0f * std::log10(aliasNoAA / std::max(aliasAA, 1e-9f)));
        check(aliasAA < aliasNoAA * 0.1f, "anti-alias filter cuts the fold-back by 20 dB+");
    }

    printf("elephant: sub-bass must not duck the audible band\n");
    {
        // -17 st puts a 120 Hz voice near 41 Hz, which the mask speaker cannot
        // reproduce. The output highpass removes it. Measured across a 20 dB
        // range of input levels this costs nothing audible (the only loss is
        // the filter's own skirt, ~0.4 dB at 250 Hz) -- note it does NOT make
        // the audible band louder: the soft clipper ahead of the limiter
        // already governs the envelope, so the discarded sub-bass was not
        // buying gain reduction the way one might assume.
        float audible[2] = {0.0f, 0.0f}, sub[2] = {0.0f, 0.0f};

        for (int withHp = 0; withHp < 2; ++withHp) {
            dsp::DCBlocker dc;
            dsp::Biquad hp, lp, outHp;
            dsp::NoiseGate gate;
            dsp::PitchShifter ps;
            dsp::RingMod ring;
            dsp::Reverb rev;
            dsp::Limiter lim;

            hp.highpass(120.0f, 0.707f, FS);
            lp.lowpass(2600.0f, 0.707f, FS);
            outHp.highpass(120.0f, 0.707f, FS);
            gate.init(FS);
            gate.setThreshold(0.006f);
            ps.setRatio(std::pow(2.0f, -17.0f / 12.0f));
            ps.reset();
            ring.setFreq(18.0f, FS);
            rev.init(FS);
            lim.init(FS);

            const float drive = 5.5f;   // no make-up attenuation, as in the firmware
            std::vector<float> out((int)FS * 2);
            for (size_t i = 0; i < out.size(); ++i) {
                const float t = i / FS;
                float x = 0.05f * (std::sin(2.0f * dsp::kPi * 120.0f * t) +
                                   0.6f * std::sin(2.0f * dsp::kPi * 480.0f * t) +
                                   0.3f * std::sin(2.0f * dsp::kPi * 1500.0f * t));
                x = dc.process(x * 6.0f);
                x = hp.process(x);
                x = gate.process(x);
                float y = ps.process(x);
                y = y * 0.70f + y * ring.next() * 0.30f;
                y = dsp::softClip(y * drive);
                y = lp.process(y);
                y += rev.process(y) * 0.28f;
                if (withHp) y = outHp.process(y);
                out[i] = lim.process(y * 0.85f);
            }

            // Split the tail of the run into "what the speaker can render"
            // and "what it cannot", with steep-ish 4th-order splits.
            dsp::Biquad a1, a2, s1, s2;
            a1.highpass(250.0f, 0.707f, FS);
            a2.highpass(250.0f, 0.707f, FS);
            s1.lowpass(90.0f, 0.707f, FS);
            s2.lowpass(90.0f, 0.707f, FS);
            double ae = 0.0, se = 0.0;
            int n = 0;
            for (size_t i = 0; i < out.size(); ++i) {
                const float a = a2.process(a1.process(out[i]));
                const float b = s2.process(s1.process(out[i]));
                if (i > FS) {            // let the reverb and limiter settle
                    ae += a * a;
                    se += b * b;
                    ++n;
                }
            }
            audible[withHp] = std::sqrt(ae / n);
            sub[withHp] = std::sqrt(se / n);
            check(finite(out), withHp ? "no NaN/Inf with highpass" : "no NaN/Inf without");
            check(peak(out, (int)FS) <= 0.95f, "output stays below full scale");
        }

        printf("  below 90 Hz : %.5f -> %.5f (%.1f dB removed)\n", sub[0], sub[1],
               20.0f * std::log10(sub[0] / std::max(sub[1], 1e-9f)));
        printf("  above 250 Hz: %.5f -> %.5f (%+.1f dB audible)\n", audible[0], audible[1],
               20.0f * std::log10(audible[1] / std::max(audible[0], 1e-9f)));
        check(sub[1] < sub[0] * 0.5f, "sub-bass the speaker cannot use is removed");
        check(audible[1] > audible[0] * 0.9f, "audible band survives intact (within ~1 dB)");
    }

    printf("reverb\n");
    {
        dsp::Reverb rv;
        rv.init(FS);
        rv.setDecay(0.85f);
        std::vector<float> out((int)(FS * 5));
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = rv.process(i == 0 ? 1.0f : 0.0f);
        }
        float e1 = 0.0f, e2 = 0.0f;
        for (int i = 0; i < 16000; ++i) e1 += out[i] * out[i];
        for (size_t i = out.size() - 16000; i < out.size(); ++i) e2 += out[i] * out[i];
        printf("  energy first 0.5 s %.4f, last 0.5 s %.6f\n", e1, e2);
        check(finite(out), "no NaN/Inf");
        check(e2 < e1 * 0.05f, "tail decays");
    }

    printf("gate\n");
    {
        dsp::NoiseGate g;
        g.init(FS);
        g.setThreshold(0.01f);
        float quiet = 0.0f, loud = 0.0f;
        for (int i = 0; i < (int)FS; ++i) {
            quiet = g.process(0.002f * std::sin(2.0f * dsp::kPi * 300.0f * i / FS));
        }
        for (int i = 0; i < (int)FS; ++i) {
            loud = g.process(0.30f * std::sin(2.0f * dsp::kPi * 300.0f * i / FS));
        }
        printf("  quiet out %.6f, loud out %.4f\n", std::fabs(quiet), std::fabs(loud));
        check(std::fabs(quiet) < 1e-4f, "gate closes on noise floor");
    }

    printf("limiter\n");
    {
        dsp::Limiter lim;
        lim.init(FS);
        std::vector<float> out((int)FS);
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = lim.process(3.0f * std::sin(2.0f * dsp::kPi * 150.0f * i / FS));
        }
        const float p = peak(out, (int)FS / 4);
        printf("  peak with 3x overdrive: %.3f\n", p);
        check(p <= 0.95f, "output stays below full scale");
        check(finite(out), "no NaN/Inf");
    }

    printf("full chain (Demon preset, 1 s of speech-ish input)\n");
    {
        dsp::DCBlocker dc;
        dsp::Biquad hp, lp;
        dsp::NoiseGate gate;
        dsp::PitchShifter ps1;
        dsp::RingMod ring;
        dsp::Reverb rev;
        dsp::Limiter lim;

        hp.highpass(120.0f, 0.707f, FS);
        lp.lowpass(4000.0f, 0.707f, FS);
        gate.init(FS);
        gate.setThreshold(0.006f);
        ps1.reset();
        ps1.setRatio(std::pow(2.0f, -7.0f / 12.0f));
        ring.setFreq(25.0f, FS);
        rev.init(FS);
        lim.init(FS);

        const float drive = 2.5f;   // no make-up attenuation, as in the firmware
        std::vector<float> out((int)FS);
        for (size_t i = 0; i < out.size(); ++i) {
            // glottal-ish buzz + formant-ish overtones at -30 dBFS
            const float t = i / FS;
            float in = 0.03f * (std::sin(2.0f * dsp::kPi * 120.0f * t) +
                                0.5f * std::sin(2.0f * dsp::kPi * 600.0f * t) +
                                0.25f * std::sin(2.0f * dsp::kPi * 2400.0f * t));
            float x = in * 6.0f;
            x = dc.process(x);
            x = hp.process(x);
            x = gate.process(x);
            float y = ps1.process(x);
            y = y * 0.9f + x * 0.1f;
            y = y * 0.78f + y * ring.next() * 0.22f;
            y = dsp::softClip(y * drive);
            y = lp.process(y);
            y += rev.process(y) * 0.12f;
            out[i] = lim.process(y * 0.9f);
        }
        const float p = peak(out, (int)FS / 2);
        printf("  output peak %.3f\n", p);
        check(finite(out), "no NaN/Inf");
        check(p > 0.05f, "signal actually gets through");
        check(p <= 0.95f, "no clipping past full scale");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
