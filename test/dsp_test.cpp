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

        const float drive = 2.5f, comp = 1.0f / (0.5f + 0.5f * drive);
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
            y = dsp::softClip(y * drive) * comp;
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
