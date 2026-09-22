// Halloween demon-voice mask
//
//   ICS-43434 (I2S MEMS mic) -> ESP32 DSP -> MAX98357A (I2S class-D amp)
//
// Both peripherals are I2S slaves and share BCLK/WS, so a single I2S
// controller runs in full-duplex mode: one clock, no capture/playback drift,
// two pins saved. Audio runs in its own pinned task on core 1; the UI
// (button, LED, battery, console) lives on core 0.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "config.h"
#include "dsp.h"
#include "presets.h"

static const char* TAG = "mask";

// --------------------------------------------------------------- state ----

struct Params {
    float pitch1 = -7.0f;
    float pitch2 = 0.0f;
    float mix2 = 0.0f;
    float dryMix = 0.10f;
    float drive = 2.5f;
    float ringHz = 0.0f;
    float ringMix = 0.0f;
    float reverbMix = 0.12f;
    float lpHz = 4000.0f;
    float hpHz = 120.0f;
    float inGain = 6.0f;      // ICS-43434 speech sits around -30 dBFS
    float outGain = 0.9f;
    float gateThr = 0.006f;
    bool muted = false;
};

static Params s_params;                  // owned by the audio task
static Params s_pending;                 // written by the UI task
static volatile bool s_pendingDirty = false;
static SemaphoreHandle_t s_paramLock;

static volatile int s_preset = 0;
static volatile int s_micSlot = MIC_SLOT_DEFAULT;
static volatile bool s_meter = false;

// Telemetry (single-writer floats; a torn read would only smudge a printout)
static volatile float s_cpuLoad = 0.0f;
static volatile float s_peakIn = 0.0f;
static volatile float s_peakOut = 0.0f;
// Raw 24-bit extremes straight off the I2S bus, before any gain, for BOTH
// slots regardless of which one we are listening to. A mic that is merely
// quiet still dithers by a few counts, so a dead flat 0..0 on both slots
// means no data is arriving on DIN at all - a clock, ground or wiring fault,
// not a slot-selection mistake.
static volatile int32_t s_rawMinL = 0;
static volatile int32_t s_rawMaxL = 0;
static volatile int32_t s_rawMinR = 0;
static volatile int32_t s_rawMaxR = 0;
static volatile int s_vbatMv = 0;

static i2s_chan_handle_t s_tx = nullptr;
static i2s_chan_handle_t s_rx = nullptr;

// DSP chain
static dsp::DCBlocker s_dc;
static dsp::Biquad s_hp;
static dsp::Biquad s_lp;
static dsp::NoiseGate s_gate;
static dsp::PitchShifter s_ps1;
static dsp::PitchShifter s_ps2;
static dsp::RingMod s_ring;
static dsp::Reverb s_reverb;
static dsp::Limiter s_limiter;

static float s_driveComp = 1.0f;

// Bring-up test tone: bypasses the whole chain and the mic entirely.
static dsp::RingMod s_toneOsc;
static volatile int s_toneFrames = 0;    // counts down, 0 = off
static volatile bool s_toneHold = false; // console-latched, runs until cleared

// ----------------------------------------------------------- parameters ----

static void applyParams(const Params& p) {
    s_ps1.setRatio(powf(2.0f, p.pitch1 / 12.0f));
    s_ps2.setRatio(powf(2.0f, p.pitch2 / 12.0f));
    s_hp.highpass(p.hpHz, 0.707f, SAMPLE_RATE);
    s_lp.lowpass(p.lpHz, 0.707f, SAMPLE_RATE);
    s_ring.setFreq(p.ringHz, SAMPLE_RATE);
    s_gate.setThreshold(p.gateThr);
    s_reverb.setDecay(0.70f + 0.20f * dsp::clampf(p.reverbMix * 3.0f, 0.0f, 1.0f));
    // Keep perceived loudness roughly constant as drive goes up.
    s_driveComp = 1.0f / (0.5f + 0.5f * p.drive);
    s_params = p;
}

static void loadPreset(int idx) {
    if (idx < 0 || idx >= kPresetCount) return;
    const Preset& pr = kPresets[idx];
    xSemaphoreTake(s_paramLock, portMAX_DELAY);
    s_pending.pitch1 = pr.pitch1;
    s_pending.pitch2 = pr.pitch2;
    s_pending.mix2 = pr.mix2;
    s_pending.dryMix = pr.dryMix;
    s_pending.drive = pr.drive;
    s_pending.ringHz = pr.ringHz;
    s_pending.ringMix = pr.ringMix;
    s_pending.reverbMix = pr.reverbMix;
    s_pending.lpHz = pr.lpHz;
    s_pending.outGain = pr.outGain;
    s_pendingDirty = true;
    xSemaphoreGive(s_paramLock);
    s_preset = idx;
    ESP_LOGI(TAG, "preset %d: %s", idx, pr.name);
}

// Mutate one live parameter without touching the rest.
template <typename Fn>
static void editParams(Fn fn) {
    xSemaphoreTake(s_paramLock, portMAX_DELAY);
    fn(s_pending);
    s_pendingDirty = true;
    xSemaphoreGive(s_paramLock);
}

// ------------------------------------------------------------- audio io ----

static void i2sInit() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = DMA_DESC_NUM;
    chanCfg.dma_frame_num = AUDIO_BLOCK;
    chanCfg.auto_clear = true;   // emit silence instead of garbage on underrun
    ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &s_tx, &s_rx));

    // 32-bit stereo slots: the mic needs 24 bits of the frame, and reading
    // both slots (instead of the driver mono mode, which is quirky on the
    // original ESP32) lets us pick the microphone channel in software.
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

// ------------------------------------------------------------ processing ---

static void processBlock(const int32_t* in, int32_t* out, int frames) {
    const Params p = s_params;
    const int slot = s_micSlot;
    const bool useSecond = p.mix2 > 0.001f;
    const bool useRing = p.ringMix > 0.001f;
    const bool useReverb = p.reverbMix > 0.001f;

    float peakIn = 0.0f, peakOut = 0.0f;

    // Test tone wins over everything: if this is silent, the fault is in the
    // amp, its supply or the speaker, not in the mic or the DSP.
    if (s_toneHold || s_toneFrames > 0) {
        for (int i = 0; i < frames; ++i) {
            const float y = TEST_TONE_LEVEL * s_toneOsc.next();
            const float mag = fabsf(y);
            if (mag > peakOut) peakOut = mag;
            const int32_t o = static_cast<int32_t>(y * 8388607.0f) << 8;
            out[2 * i] = o;
            out[2 * i + 1] = o;
        }
        if (s_toneFrames > 0) {
            s_toneFrames = (s_toneFrames > frames) ? s_toneFrames - frames : 0;
        }
        s_peakOut = peakOut;
        return;
    }

    int32_t minL = INT32_MAX, maxL = INT32_MIN, minR = INT32_MAX, maxR = INT32_MIN;

    for (int i = 0; i < frames; ++i) {
        // The ICS-43434 sends 24 bits MSB-first, left-justified in the slot.
        // Watch both slots so a wrong MIC_SLOT_DEFAULT cannot hide a live mic.
        const int32_t rawL = in[2 * i] >> 8;
        const int32_t rawR = in[2 * i + 1] >> 8;
        if (rawL < minL) minL = rawL;
        if (rawL > maxL) maxL = rawL;
        if (rawR < minR) minR = rawR;
        if (rawR > maxR) maxR = rawR;

        const int32_t raw = slot == 0 ? rawL : rawR;
        float x = static_cast<float>(raw) * (1.0f / 8388608.0f);

        const float a = fabsf(x);
        if (a > peakIn) peakIn = a;

        x *= p.inGain;
        x = s_dc.process(x);
        x = s_hp.process(x);
        x = s_gate.process(x);

        // Pitch: one or two detuned voices, plus a little dry signal so
        // consonants stay intelligible through the mask.
        float y = s_ps1.process(x);
        if (useSecond) {
            y = y * (1.0f - p.mix2) + s_ps2.process(x) * p.mix2;
        }
        if (p.dryMix > 0.001f) {
            y = y * (1.0f - p.dryMix) + x * p.dryMix;
        }

        if (useRing) {
            y = y * (1.0f - p.ringMix) + y * s_ring.next() * p.ringMix;
        }

        y = dsp::softClip(y * p.drive) * s_driveComp;
        y = s_lp.process(y);

        if (useReverb) {
            y += s_reverb.process(y) * p.reverbMix;
        }

        y = s_limiter.process(y * p.outGain);

        if (p.muted) y = 0.0f;

        const float b = fabsf(y);
        if (b > peakOut) peakOut = b;

        const int32_t s24 = static_cast<int32_t>(dsp::clampf(y, -0.999f, 0.999f) * 8388607.0f);
        const int32_t o = s24 << 8;          // back to left-justified 32-bit
        out[2 * i] = o;                      // duplicate into both slots so the
        out[2 * i + 1] = o;                  // amp SD_MODE setting does not matter
    }

    s_peakIn = peakIn;
    s_peakOut = peakOut;
    s_rawMinL = minL;
    s_rawMaxL = maxL;
    s_rawMinR = minR;
    s_rawMaxR = maxR;
}

static void audioTask(void*) {
    static int32_t rxBuf[AUDIO_BLOCK * 2];
    static int32_t txBuf[AUDIO_BLOCK * 2];
    const float blockUs = 1e6f * AUDIO_BLOCK / SAMPLE_RATE;

    while (true) {
        size_t bytesRead = 0;
        if (i2s_channel_read(s_rx, rxBuf, sizeof(rxBuf), &bytesRead,
                             pdMS_TO_TICKS(200)) != ESP_OK) {
            ESP_LOGW(TAG, "i2s read timeout");
            continue;
        }
        const int frames = bytesRead / (2 * sizeof(int32_t));
        if (frames <= 0) continue;

        const int64_t t0 = esp_timer_get_time();

        if (s_pendingDirty && xSemaphoreTake(s_paramLock, 0) == pdTRUE) {
            const Params p = s_pending;
            s_pendingDirty = false;
            xSemaphoreGive(s_paramLock);
            applyParams(p);
        }

        processBlock(rxBuf, txBuf, frames);

        const int64_t t1 = esp_timer_get_time();
        s_cpuLoad = 0.95f * s_cpuLoad + 0.05f * (100.0f * (t1 - t0) / blockUs);

        size_t bytesWritten = 0;
        i2s_channel_write(s_tx, txBuf, frames * 2 * sizeof(int32_t), &bytesWritten,
                          pdMS_TO_TICKS(200));
    }
}

// --------------------------------------------------------------- battery ---

#if BATTERY_MONITOR
static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;

// ADC_ATTEN_DB_11 was renamed to ADC_ATTEN_DB_12 in IDF 5.2 (the old name is
// an enumerator, not a macro, so this has to be a version check).
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
#define MASK_ADC_ATTEN ADC_ATTEN_DB_11
#else
#define MASK_ADC_ATTEN ADC_ATTEN_DB_12
#endif

static void batteryInit() {
    adc_oneshot_unit_init_cfg_t unitCfg = {};
    unitCfg.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unitCfg, &s_adc));

    adc_oneshot_chan_cfg_t chanCfg = {};
    chanCfg.atten = MASK_ADC_ATTEN;
    chanCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &chanCfg));

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t caliCfg = {};
    caliCfg.unit_id = ADC_UNIT_1;
    caliCfg.atten = MASK_ADC_ATTEN;
    caliCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&caliCfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration, battery reading is approximate");
    }
#endif
}

static volatile int s_vbatRaw = 0;      // last raw ADC count, for debugging
static volatile int s_vbatPinMv = 0;    // last voltage at the pin itself

static int batteryMv() {
    int accRaw = 0, accMv = 0;
    for (int i = 0; i < 8; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_6, &raw) != ESP_OK) return 0;
        accRaw += raw;
        int mv = 0;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            accMv += mv;
        } else {
            accMv += raw * 2450 / 4095;   // rough fallback for 12 dB attenuation
        }
    }
    s_vbatRaw = accRaw / 8;
    s_vbatPinMv = accMv / 8;
    return static_cast<int>(s_vbatPinMv * VBAT_DIVIDER);
}

static bool vbatPlausible(int mv) {
    return mv >= VBAT_PLAUSIBLE_MIN_MV && mv <= VBAT_PLAUSIBLE_MAX_MV;
}

static void shutdownForLowBattery(int mv) {
    ESP_LOGE(TAG, "battery %d mV on %d consecutive reads - shutting down to protect the cell",
             mv, VBAT_CUTOFF_STRIKES);
    gpio_set_level(PIN_AMP_SD, 0);
    gpio_set_level(PIN_LED, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    // Leave a way back in without a power cycle: the button wakes it, and it
    // re-measures on boot. If the cell really is flat it will shut down again.
    rtc_gpio_pullup_en(PIN_BUTTON);
    rtc_gpio_pulldown_dis(PIN_BUTTON);
    esp_sleep_enable_ext0_wakeup(PIN_BUTTON, 0);
    esp_deep_sleep_start();
}
#endif  // BATTERY_MONITOR

// ------------------------------------------------------------- console -----

static void printHelp() {
    printf("\n--- demon mask console ---\n");
    printf("  n        next preset          0-%d  select preset\n", kPresetCount - 1);
    printf("  m        mute toggle          s    stats\n");
    printf("  x        swap mic I2S slot    l    level meter toggle\n");
    printf("  p <st>   pitch voice 1        P <st> pitch voice 2\n");
    printf("  M <0-1>  voice 2 mix          y <0-1> dry blend\n");
    printf("  d <x>    drive                f <hz>  ring mod freq\n");
    printf("  r <0-1>  ring mod mix         v <0-1> reverb mix\n");
    printf("  c <hz>   tone lowpass         g <x>   input gain\n");
    printf("  o <0-1>  output gain          t <x>   gate threshold\n");
    printf("  b        read the battery sense pin\n");
    printf("  T        test tone on/off (bypasses mic and DSP)\n");
    for (int i = 0; i < kPresetCount; ++i) {
        printf("  [%d] %s\n", i, kPresets[i].name);
    }
    printf("\n");
}

static void printStats() {
    const Params p = s_params;
    printf("preset %d (%s)  cpu %.1f%%  in %.3f  out %.3f  slot %s  %s\n",
           s_preset, kPresets[s_preset].name, s_cpuLoad, s_peakIn, s_peakOut,
           s_micSlot == 0 ? "L" : "R", p.muted ? "MUTED" : "live");
    printf("  pitch %.1f/%.1f st  mix2 %.2f  dry %.2f  drive %.1f\n",
           p.pitch1, p.pitch2, p.mix2, p.dryMix, p.drive);
    printf("  ring %.0f Hz x%.2f  reverb %.2f  lp %.0f Hz  in x%.1f  out %.2f  gate %.4f\n",
           p.ringHz, p.ringMix, p.reverbMix, p.lpHz, p.inGain, p.outGain, p.gateThr);
#if BATTERY_MONITOR
    printf("  battery %d mV (pin %d mV, raw %d)%s\n", s_vbatMv, s_vbatPinMv, s_vbatRaw,
           vbatPlausible(s_vbatMv) ? "" : "  <- no battery sense, ignored");
#endif
    printf("  free heap %u B\n", (unsigned)esp_get_free_heap_size());
}

static void handleLine(char* line) {
    while (*line == ' ') ++line;
    if (*line == 0) return;

    const char cmd = *line;
    const char* arg = line + 1;
    float val = 0.0f;
    const bool hasVal = (sscanf(arg, "%f", &val) == 1);

    switch (cmd) {
        case 'h':
        case '?':
            printHelp();
            return;
        case 's':
            printStats();
            return;
        case 'n':
            loadPreset((s_preset + 1) % kPresetCount);
            return;
        case 'x':
            s_micSlot = s_micSlot ? 0 : 1;
            printf("mic slot -> %s\n", s_micSlot == 0 ? "left" : "right");
            return;
        case 'l':
            s_meter = !s_meter;
            return;
        case 'T':
            s_toneHold = !s_toneHold;
            s_toneFrames = 0;
            printf("test tone %s (%.0f Hz, bypasses mic and DSP)\n",
                   s_toneHold ? "ON" : "off", TEST_TONE_HZ);
            return;
        case 'b':
#if BATTERY_MONITOR
            s_vbatMv = batteryMv();
            printf("battery %d mV (pin %d mV, raw %d) plausible=%s\n", s_vbatMv, s_vbatPinMv,
                   s_vbatRaw, vbatPlausible(s_vbatMv) ? "yes" : "no");
#else
            printf("battery monitor disabled at build time\n");
#endif
            return;
        case 'm':
            editParams([](Params& p) { p.muted = !p.muted; });
            gpio_set_level(PIN_AMP_SD, s_pending.muted ? 0 : 1);
            printf("%s\n", s_pending.muted ? "muted" : "live");
            return;
        default:
            break;
    }

    if (cmd >= '0' && cmd <= '9') {
        loadPreset(cmd - '0');
        return;
    }
    if (!hasVal) {
        printf("need a value, e.g. 'p -7'\n");
        return;
    }

    switch (cmd) {
        case 'p': editParams([val](Params& p) { p.pitch1 = dsp::clampf(val, -24.0f, 12.0f); }); break;
        case 'P': editParams([val](Params& p) { p.pitch2 = dsp::clampf(val, -24.0f, 12.0f); }); break;
        case 'M': editParams([val](Params& p) { p.mix2 = dsp::clampf(val, 0.0f, 1.0f); }); break;
        case 'y': editParams([val](Params& p) { p.dryMix = dsp::clampf(val, 0.0f, 1.0f); }); break;
        case 'd': editParams([val](Params& p) { p.drive = dsp::clampf(val, 1.0f, 20.0f); }); break;
        case 'f': editParams([val](Params& p) { p.ringHz = dsp::clampf(val, 0.0f, 2000.0f); }); break;
        case 'r': editParams([val](Params& p) { p.ringMix = dsp::clampf(val, 0.0f, 1.0f); }); break;
        case 'v': editParams([val](Params& p) { p.reverbMix = dsp::clampf(val, 0.0f, 0.8f); }); break;
        case 'c': editParams([val](Params& p) { p.lpHz = dsp::clampf(val, 500.0f, 12000.0f); }); break;
        case 'g': editParams([val](Params& p) { p.inGain = dsp::clampf(val, 0.1f, 64.0f); }); break;
        case 'o': editParams([val](Params& p) { p.outGain = dsp::clampf(val, 0.0f, 1.0f); }); break;
        case 't': editParams([val](Params& p) { p.gateThr = dsp::clampf(val, 0.0f, 0.5f); }); break;
        default:
            printf("unknown command (h for help)\n");
            return;
    }
    printf("ok\n");
}

static void consoleInit() {
    uart_config_t uc = {};
    uc.baud_rate = 115200;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    // Order matters: configure the port, then install the driver. Installing
    // first and reconfiguring afterwards leaves RX dead - keypresses never
    // reach uart_read_bytes even though TX keeps printing happily.
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uc));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0));
    uart_flush_input(UART_NUM_0);
}

static void pollConsole() {
    static char line[64];
    static int len = 0;
    uint8_t ch = 0;
    while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) {
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = 0;
                handleLine(line);
                len = 0;
            }
        } else if (len < static_cast<int>(sizeof(line)) - 1) {
            line[len++] = static_cast<char>(ch);
        }
    }
}

// ------------------------------------------------------------------ UI -----

static void uiTask(void*) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    bool lastBtn = true;
    int64_t pressedAt = 0;
    bool longFired = false;
    int ledPhase = 0;
    int64_t lastMeter = 0;
    bool lowBattery = false;
#if BATTERY_MONITOR
    int64_t lastBattery = 0;
    int strikes = 0;
    bool senseOk = true;
#endif

    while (true) {
        pollConsole();

        // --- button: tap = next preset, hold = mute -----------------------
        const bool btn = gpio_get_level(PIN_BUTTON) != 0;   // true = released
        const int64_t now = esp_timer_get_time();
        if (lastBtn && !btn) {
            pressedAt = now;
            longFired = false;
        } else if (!btn && !longFired && (now - pressedAt) > 800000) {
            editParams([](Params& p) { p.muted = !p.muted; });
            gpio_set_level(PIN_AMP_SD, s_pending.muted ? 0 : 1);
            ESP_LOGI(TAG, "%s", s_pending.muted ? "muted" : "live");
            longFired = true;
        } else if (!lastBtn && btn && !longFired && (now - pressedAt) > 30000) {
            loadPreset((s_preset + 1) % kPresetCount);
        }
        lastBtn = btn;

        // --- battery ------------------------------------------------------
#if BATTERY_MONITOR
        if (now - lastBattery > 2000000) {
            lastBattery = now;
            const int mv = batteryMv();
            s_vbatMv = mv;

            if (!vbatPlausible(mv)) {
                // No usable battery sense. Keep running: an unfitted divider
                // must never be able to shut the mask down.
                if (senseOk) {
                    ESP_LOGW(TAG,
                             "battery sense reads %d mV (pin %d mV, raw %d), outside %d-%d mV: "
                             "ignoring it. Divider not fitted? Set BATTERY_MONITOR 0 to silence.",
                             mv, s_vbatPinMv, s_vbatRaw, VBAT_PLAUSIBLE_MIN_MV,
                             VBAT_PLAUSIBLE_MAX_MV);
                    senseOk = false;
                }
                lowBattery = false;
                strikes = 0;
            } else {
                if (!senseOk) {
                    ESP_LOGI(TAG, "battery sense back in range: %d mV", mv);
                    senseOk = true;
                }
                lowBattery = mv < VBAT_WARN_MV;
                if (mv < VBAT_CUTOFF_MV) {
                    if (++strikes >= VBAT_CUTOFF_STRIKES) shutdownForLowBattery(mv);
                    ESP_LOGW(TAG, "battery %d mV, strike %d/%d", mv, strikes,
                             VBAT_CUTOFF_STRIKES);
                } else {
                    strikes = 0;
                }
            }
        }
#endif

        // --- LED: fast blink = low battery, solid = muted, otherwise it
        //     blinks the preset number once every two seconds --------------
        ledPhase = (ledPhase + 1) % 200;             // 10 ms tick -> 2 s cycle
        int level;
        if (lowBattery) {
            level = (ledPhase / 10) & 1;
        } else if (s_params.muted) {
            level = 1;
        } else {
            const int blinks = s_preset + 1;
            const int slotIdx = ledPhase / 20;
            level = (slotIdx < blinks * 2) ? ((slotIdx & 1) == 0) : 0;
        }
        gpio_set_level(PIN_LED, level);

        // Levels once a second, automatically for the first STARTUP_METER_SEC
        // so a board whose console RX is not working can still be diagnosed.
        const bool startupMeter = now < static_cast<int64_t>(STARTUP_METER_SEC) * 1000000;
        if ((s_meter || startupMeter) && (now - lastMeter) > 1000000) {
            lastMeter = now;
            printf("in %.4f  out %.4f  L[%ld..%ld] R[%ld..%ld] using %s  cpu %.1f%%  %s%s\n",
                   s_peakIn, s_peakOut, static_cast<long>(s_rawMinL),
                   static_cast<long>(s_rawMaxL), static_cast<long>(s_rawMinR),
                   static_cast<long>(s_rawMaxR), s_micSlot == 0 ? "L" : "R", s_cpuLoad,
                   kPresets[s_preset].name, s_toneHold || s_toneFrames > 0 ? "  TEST TONE" : "");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------- main -----

extern "C" void app_main(void) {
    s_paramLock = xSemaphoreCreateMutex();

    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << PIN_AMP_SD) | (1ULL << PIN_LED);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level(PIN_AMP_SD, 0);      // amp off until audio is flowing
    gpio_set_level(PIN_LED, 0);

    consoleInit();
#if BATTERY_MONITOR
    batteryInit();
    // Battery monitoring is advisory. On USB power, or with no sense divider
    // fitted, the reading is meaningless and is ignored - the mask still runs.
    s_vbatMv = batteryMv();
    if (vbatPlausible(s_vbatMv)) {
        ESP_LOGI(TAG, "battery %d mV", s_vbatMv);
    } else {
        ESP_LOGI(TAG, "no battery sense (%d mV on the divider) - running on external power",
                 s_vbatMv);
    }
#endif

    dsp::RingMod::buildTable();
    s_gate.init(SAMPLE_RATE);
    s_reverb.init(SAMPLE_RATE);
    s_limiter.init(SAMPLE_RATE);
    s_ps1.reset();
    s_ps2.reset();

    applyParams(s_params);
    s_pending = s_params;
    loadPreset(0);

    i2sInit();

    // Float use requires a pinned task on the ESP32 (lazy FPU context save).
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 20, nullptr, 1);
    xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 5, nullptr, 0);

    vTaskDelay(pdMS_TO_TICKS(120));     // let the DMA prime, avoids a pop
    gpio_set_level(PIN_AMP_SD, 1);
    ESP_LOGI(TAG, "amp enabled (SD_MODE high on GPIO%d)", PIN_AMP_SD);

    s_toneOsc.setFreq(TEST_TONE_HZ, SAMPLE_RATE);
#if TEST_TONE_ON_BOOT
    s_toneFrames = SAMPLE_RATE * TEST_TONE_MS / 1000;
    ESP_LOGI(TAG, "playing %.0f Hz test tone for %d ms - if you hear nothing, the "
                  "fault is the amp, its supply or the speaker, not the mic",
             TEST_TONE_HZ, TEST_TONE_MS);
#endif

    ESP_LOGI(TAG, "running at %d Hz, %d-frame blocks (%.1f ms)", SAMPLE_RATE,
             AUDIO_BLOCK, 1000.0f * AUDIO_BLOCK / SAMPLE_RATE);
    printHelp();
}
