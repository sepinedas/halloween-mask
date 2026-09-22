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
};

// Tuned by ear in a latex mask with a 4 ohm 3 W speaker. The console 'p/d/r'
// commands edit the live copy so you can dial these in without reflashing.
static const Preset kPresets[] = {
    // name            p1     p2   mix2  dry  drive  ringHz rmix  rev   lp     out
    {"Demon",         -7.0f,  0.0f, 0.0f, 0.10f, 2.5f,   0.0f, 0.00f, 0.12f, 4000.0f, 0.9f},
    {"Deep Demon",   -12.0f,  0.0f, 0.0f, 0.06f, 4.0f,  25.0f, 0.22f, 0.18f, 3200.0f, 0.9f},
    {"Ghoul Choir",   -5.0f, -12.0f, 0.5f, 0.10f, 1.8f,  0.0f, 0.00f, 0.30f, 4500.0f, 0.85f},
    {"Possessed",     -9.0f,  -4.0f, 0.3f, 0.00f, 6.0f, 61.0f, 0.40f, 0.25f, 3000.0f, 0.8f},
    {"Clean (test)",   0.0f,   0.0f, 0.0f, 1.00f, 1.0f,  0.0f, 0.00f, 0.00f, 7000.0f, 0.9f},
    // Squirrel: a full octave up, kept clean and bright. No dry blend - any
    // of the original voice underneath ruins the illusion - and no reverb,
    // because a squirrel is not in a crypt. The pitch-up aliasing is handled
    // by the input anti-alias filter, not by dulling the output.
    {"Squirrel",      12.0f,   0.0f, 0.0f, 0.00f, 1.2f,  0.0f, 0.00f, 0.04f, 8000.0f, 0.85f},
    // Elephant: a rumble, not a trumpet. 17 semitones down puts a male voice
    // near 40 Hz, which no mask speaker will ever reproduce - so the heavy
    // drive is doing real work here, generating harmonics the speaker can
    // render while the ear reconstructs the missing fundamental. The 18 Hz
    // ring modulation is the flutter that makes a big animal sound big.
    {"Elephant",     -17.0f,   0.0f, 0.0f, 0.00f, 5.5f, 18.0f, 0.30f, 0.28f, 2600.0f, 0.85f},
};

static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
