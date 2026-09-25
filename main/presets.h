// Voice presets. Cycle with the button, or select with 0..4 on the console.
#pragma once

struct Preset {
    const char* name;
    float pitch1;     // semitones, main voice
    float pitch2;     // semitones, second voice
    float mix2;       // 0 = second voice off
    float dryMix;     // un-shifted voice blended back in (keeps consonants)
    float drive;      // pre-waveshaper gain
    float ringHz;     // ring modulator frequency
    float ringMix;    // 0 = off
    float reverbMix;
    float lpHz;       // post-distortion tone control
    float outGain;
    float bodyDb;     // broad lift around 600 Hz, 0 = off (see Woman)
};

// Tuned by ear in a latex mask with a 4 ohm 3 W speaker. The console 'p/d/r'
// commands edit the live copy so you can dial these in without reflashing.
static const Preset kPresets[] = {
    // name            p1     p2   mix2  dry  drive  ringHz rmix  rev   lp     out   body
    {"Demon",         -7.0f,  0.0f, 0.0f, 0.10f, 2.5f,  0.0f, 0.00f, 0.12f, 4000.0f, 1.0f,  0.0f},
    {"Deep Demon",   -12.0f,  0.0f, 0.0f, 0.06f, 4.0f, 25.0f, 0.22f, 0.18f, 3200.0f, 1.0f,  0.0f},
    // Woman: a compromise, and worth knowing why. Pitch alone would want about
    // +7 semitones, but this shifter resamples, so formants rise by the same
    // factor - and formants that high read as a child rather than an adult
    // woman, because a woman's vocal tract is around 15% shorter than a man's,
    // not 50%. So: a smaller +5 st shift, no dry blend, and a broad lift near
    // 600 Hz to put back some of the chest the upward shift thins out.
    {"Woman",          5.0f,  0.0f, 0.0f, 0.00f, 1.1f,  0.0f, 0.00f, 0.06f, 7000.0f, 1.0f,  4.0f},
    // Squirrel: a full octave up, kept clean and bright. No dry blend - any
    // of the original voice underneath ruins the illusion - and no reverb,
    // because a squirrel is not in a crypt. The pitch-up aliasing is handled
    // by the input anti-alias filter, not by dulling the output. Here the
    // formants riding up with the pitch is exactly the point.
    {"Squirrel",      12.0f,  0.0f, 0.0f, 0.00f, 1.2f,  0.0f, 0.00f, 0.04f, 8000.0f, 1.0f,  0.0f},
    {"Clean (test)",   0.0f,  0.0f, 0.0f, 1.00f, 1.0f,  0.0f, 0.00f, 0.00f, 7000.0f, 1.0f,  0.0f},
};

static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
