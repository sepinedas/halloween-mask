// Board / audio configuration for the demon-voice mask.
#pragma once

#include "hal/gpio_types.h"

// ---------------------------------------------------------------- pins ----
// The mic and the amp are both I2S slaves, so they share BCLK and WS and we
// run a single I2S peripheral in full-duplex mode. One clock domain means
// capture and playback can never drift apart.
#define PIN_I2S_BCLK      GPIO_NUM_26   // ICS-43434 SCK  + MAX98357A BCLK
#define PIN_I2S_WS        GPIO_NUM_25   // ICS-43434 WS   + MAX98357A LRC
#define PIN_I2S_DIN       GPIO_NUM_33   // ICS-43434 SD   -> ESP32
#define PIN_I2S_DOUT      GPIO_NUM_22   // ESP32 -> MAX98357A DIN

#define PIN_AMP_SD        GPIO_NUM_21   // MAX98357A SD_MODE: low = shutdown
#define PIN_BUTTON        GPIO_NUM_4    // to GND, internal pull-up
#define PIN_LED           GPIO_NUM_2    // on-board LED on most dev boards
#define PIN_VBAT          GPIO_NUM_34   // ADC1_CH6, 100k/100k divider from B+

// --------------------------------------------------------------- audio ----
// The ICS-43434 is only in spec between 23 kHz and 51.6 kHz; below that it
// drops into a low-power mode and the output goes quiet/noisy. 32 kHz gives
// plenty of speech bandwidth and leaves the CPU almost idle.
#define SAMPLE_RATE       32000
#define AUDIO_BLOCK       128           // frames per DMA buffer -> 4 ms
#define DMA_DESC_NUM      4             // ~16 ms of buffering each way

// Which I2S slot the mic lands in. With the ICS-43434 L/R pin tied to GND it
// is the left slot (0). Tie L/R to VDD (or set this to 1) for the right slot.
// Runtime-togglable with the 'x' console command while debugging.
#define MIC_SLOT_DEFAULT  0

// ------------------------------------------------------------- bring-up ----
// Test tone, toggled with 'T' on the console. It bypasses the microphone and
// the whole DSP chain, so it isolates the output half of the board: if the
// tone is audible, the amp, its supply, the speaker and the I2S TX path are
// all good and any remaining silence is the microphone's fault.
#define TEST_TONE_HZ      440.0f
#define TEST_TONE_LEVEL   0.25f

// Print input/output levels once a second for this long after boot, so the
// mask can be brought up on a monitor that only shows output. 0 disables.
#define STARTUP_METER_SEC 20

// ------------------------------------------------------------- loudness ----
// Gain into the limiter, and the level the limiter holds the output to.
//
// Once peaks are already at the rail, "louder" means raising the average, and
// the only way to do that is to push more signal into gain reduction. This is
// that push. It does not raise peaks - the limiter normalises whatever it is
// handed to LIMITER_CEILING - it raises RMS by flattening the crest factor.
//
// Measured, it is worth +0.8 dB on Demon (already limiting, so it gains only
// what the ceiling gives) up to +6 dB on Clean, and it brings every preset to
// roughly the same loudness, which is a pleasant side effect. Past about 6 dB
// it does nothing whatsoever: the limiter absorbs all of it, and +12 dB
// measures identically to +6.
//
// The cost is compression. Quiet passages come up with everything else, so the
// noise floor rises and feedback inside a mask gets more likely. Back this off
// first if it howls.
#define LOUDNESS_DB     6.0f
#define LIMITER_CEILING 0.95f

// -------------------------------------------------------------- speaker ----
// A 40 mm mask speaker makes nothing useful below roughly this, but a deeply
// pitched preset happily sends it 40 Hz anyway: energy that only moves the
// cone and eats into the amp's ~3.2 W. Highpassing the output discards it and
// lets the harmonics carry the perceived pitch instead - the ear reconstructs
// a missing fundamental quite happily. Measured on a 17-semitone-down voice
// it removes ~15 dB below 90 Hz and costs 0.4 dB above 250 Hz.
//
// It sits ahead of the limiter so discarded energy cannot contribute to gain
// reduction, though measurement says that buys little in practice: the soft
// clipper already governs the envelope. Set to 0 to disable.
#define SPEAKER_HP_HZ 120

// -------------------------------------------------------------- battery ----
#ifndef BATTERY_MONITOR                 // -DBATTERY_MONITOR=0 also works
#define BATTERY_MONITOR   1             // 0 if you did not fit the divider
#endif
#define VBAT_DIVIDER      2.0f          // 100k/100k
#define VBAT_WARN_MV      3400          // start warning (LED fast blink)
#define VBAT_CUTOFF_MV    3050          // mute + deep sleep, protects the cell

// A reading outside this window is not a 1S Li-ion, it is an unfitted or
// mis-wired divider: GPIO34 is input-only with no internal pull, so floating
// it reads a few hundred mV of noise. Anything below the minimum also cannot
// be a running system - a cell that low is already latched off by its own
// protection board and the 3V3 regulator would have dropped out. So readings
// out here mean "no battery sense", never "flat battery", and must not
// trigger the shutdown.
#define VBAT_PLAUSIBLE_MIN_MV 2500
#define VBAT_PLAUSIBLE_MAX_MV 4500

// Consecutive plausible sub-cutoff reads (2 s apart) before shutting down, so
// a bass transient sagging the rail cannot kill a working mask mid-sentence.
#define VBAT_CUTOFF_STRIKES 5
