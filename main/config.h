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

// -------------------------------------------------------------- battery ----
#define BATTERY_MONITOR   1             // 0 if you did not fit the divider
#define VBAT_DIVIDER      2.0f          // 100k/100k
#define VBAT_WARN_MV      3400          // start warning (LED fast blink)
#define VBAT_CUTOFF_MV    3050          // mute + deep sleep, protects the cell
