# Halloween demon-voice mask

Real-time voice changer for an ESP-WROOM-32: an **ICS-43434** I2S MEMS mic goes in,
a **MAX98357A** I2S class-D amp comes out, and in between the ESP32 pitches your
voice down, drives it into soft clipping and drops it in a crypt. Runs off a single
**18650** cell. Five presets, cycled with one button.

Built with **ESP-IDF 5.x** (tested against the 5.2/5.3 API surface).

---

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-WROOM-32 dev board | Any devkit-C style board |
| ICS-43434 breakout | I2S MEMS mic, 24-bit |
| MAX98357A breakout | I2S mono class-D amp, 3.2 W |
| Speaker | 4 Ω, 3 W, 40 mm is a good size for a mask |
| 18650 cell + holder | Use a **protected** cell |
| TP4056 charger module | The version with DW01 protection |
| Boost converter (MT3608 or similar) | Battery to 5 V, see power below |
| 2 × 100 kΩ | Battery sense divider |
| Momentary push button | Preset / mute |
| 470–1000 µF electrolytic | Bulk cap across the amp supply |

### Wiring

Mic and amp are both I2S slaves, so they **share BCLK and WS** — the firmware runs
one I2S controller in full-duplex mode. One clock domain, no capture/playback drift.

| ESP32 | ICS-43434 | MAX98357A | Other |
| --- | --- | --- | --- |
| GPIO26 | SCK | BCLK | |
| GPIO25 | WS | LRC | |
| GPIO33 | SD (data out) | | mic → ESP32 |
| GPIO22 | | DIN | ESP32 → amp |
| GPIO21 | | SD_MODE | high = on, low = shutdown |
| GPIO4 | | | button to GND (internal pull-up) |
| GPIO2 | | | on-board LED |
| GPIO34 | | | battery divider midpoint |
| 3V3 | VDD | | amp Vin goes to 5 V, not 3V3 |
| GND | GND, **L/R** | GND | |

Two details that will cost you an evening if you miss them:

- **Tie the mic's L/R pin to GND.** That puts its data in the left I2S slot, which is
  what `MIC_SLOT_DEFAULT` expects. If you hear nothing, press `x` on the console to
  read the right slot instead.
- The amp's **GAIN** pin sets the analog gain: floating = 9 dB, to GND = 12 dB,
  100 kΩ to GND = 15 dB. Start floating.

### Power

```
18650 ──► TP4056 (+protection) ──┬──► MT3608 boost 5V ──┬──► MAX98357A Vin
           (charge via USB)      │                      └──► ESP32 5V/VIN pin
                                 │
                                 └──► 100k ──┬── GPIO34
                                             │
                                            100k
                                             │
                                            GND
```

- The amp wants 5 V to make its rated 3.2 W. You *can* run it straight off the cell
  (2.5–5.5 V is in spec) and skip the boost, but it gets noticeably quieter as the
  cell drains — then feed the ESP32's **3V3 pin** from a low-dropout regulator, since
  a devkit's AMS1117 needs ~4.7 V on VIN and will brown out.
- Put the **bulk cap right at the amp's Vin**. Class-D bass transients pulling
  through a boost converter are the classic cause of random ESP32 resets.
- The firmware warns below 3.4 V and shuts down into deep sleep at 3.05 V to protect
  the cell. Set `BATTERY_MONITOR 0` in `main/config.h` if you skip the divider —
  otherwise a floating ADC pin may read as a flat battery.

---

## Build and flash

```bash
idf.py set-target esp32 && idf.py build && idf.py -p COM5 flash monitor
```

> I could not build or flash this here — ESP-IDF isn't installed on this machine.
> The DSP is verified by host tests (below); `main.cpp` is type-checked only, against
> stub headers mirroring the IDF 5.x API. Expect to fix a typo or two on first build.

---

## Using it

**Button (GPIO4)**

- tap → next preset
- hold ~1 s → mute (also pulls the amp into shutdown)

**LED (GPIO2)** blinks the preset number every 2 s; solid = muted; fast blink = low battery.

**Presets**

| # | Name | What it is |
| --- | --- | --- |
| 0 | Demon | −7 semitones, moderate drive. The default. |
| 1 | Deep Demon | −12 semitones, heavy drive, 25 Hz growl |
| 2 | Ghoul Choir | Two voices (−5 and −12) with a long tail |
| 3 | Possessed | −9 + −4, hard drive, 61 Hz ring mod |
| 4 | Clean | No shift, no effects — use this to test wiring |

**Serial console** at 115200 — tune by ear without reflashing, then copy the numbers
you like into `main/presets.h`. Press `h` for the list:

```
p -9      pitch voice 1, semitones      d 4.0    drive
P -12     pitch voice 2                 f 30     ring mod frequency
M 0.5     voice 2 mix                   r 0.3    ring mod mix
y 0.1     dry blend                     v 0.2    reverb mix
c 3500    tone lowpass, Hz              g 8      input gain
o 0.9     output gain                   t 0.006  gate threshold
s         stats (CPU load, levels)      l        live level meter
x         swap mic I2S slot             m        mute
```

---

## How it works

```
mic ─► gain ─► DC block ─► highpass ─► noise gate ─┬─► pitch shift 1 ─┐
                                                   ├─► pitch shift 2 ─┤
                                                   └─── dry ──────────┴─►
       ─► ring mod ─► soft clip ─► tone lowpass ─► reverb ─► limiter ─► amp
```

32 kHz, 128-sample blocks, all float. **The sample rate is not arbitrary:** the
ICS-43434 is only in spec from 23 kHz to 51.6 kHz and drops into a low-power mode
below that, so the usual 16 kHz voice rate does not work with this mic.

The pitch shifter is **WSOLA** on a delay line. A read pointer running at `ratio`
times write speed does the shifting; when it has drifted a grain's worth it must jump
back, and *where* it jumps is the whole game. Jumping a fixed distance and
crossfading — what most small pitch shifters do — lands at an arbitrary point in the
waveform. Measured on a 220 Hz tone down 7 semitones, that suppressed the wanted
146.8 Hz by 28 dB and left only two sidebands 10 Hz either side: amplitude
modulation, not pitch shifting. Searching ±9 ms for the best waveform match instead
puts the carrier back at full amplitude with sidebands ~32 dB down. The search runs
once per grain (10–25 times a second), not per sample.

The rest is a noise gate with hysteresis (it also breaks the feedback loop between a
speaker on the outside of a mask and a mic on the inside), a `tanh`-ish waveshaper,
three damped combs into two allpasses for the crypt, and a limiter so the amp never
sees a hard-clipped burst.

Audio runs at priority 20 pinned to core 1 — floats need a pinned task on the ESP32,
which saves FPU context lazily. The UI sits on core 0. No WiFi, no Bluetooth: saves
~80 mA and keeps the radios out of the audio task's way.

**Latency** is roughly 30–60 ms: ~4 ms per DMA buffer, four each way, plus up to one
32 ms grain in the shifter. Not measured on hardware. Drop `DMA_DESC_NUM` to 3 or
`AUDIO_BLOCK` to 64 in `config.h` to trade interrupt rate for latency.

---

## Tests

The DSP has no framework dependencies, so it runs on a PC:

```bash
cd test && g++ -std=c++17 -O2 -I../main -o dsp_test dsp_test.cpp ../main/dsp.cpp && ./dsp_test
```

Checks pitch ratios against a spectral estimate, guards the sideband regression
described above, and checks the reverb decays, the gate closes, and the limiter holds
below full scale. All pass. The firmware also reports its own CPU load — press `s`.

---

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Silence, `in` level is 0.000 (`l`) | Mic in the other I2S slot — press `x`, or check L/R is grounded |
| Silence, `in` moves but `out` is 0 | Muted, or SD_MODE not pulled high |
| Hiss but no voice | Gate threshold too high: `t 0.002` |
| Distorted even on preset 4 | Input gain too high: `g 3` |
| Howling feedback | Move the speaker off-axis from the mic, raise the gate (`t 0.01`), lower `o` |
| Voice sounds warbly/robotic | Input too quiet for the WSOLA search to lock — raise `g` |
| Random reboots on loud bass | Bulk cap missing at the amp, or the boost converter is sagging |
| Quiet and getting quieter | Cell is draining; check `s` for the battery reading |

## Safety

- Use a protected 18650 and a charger module with protection. Never charge an
  unattended cell inside a mask.
- Class-D at 3 W a few centimetres from your ears is genuinely loud. Set `o` low
  first, and keep the speaker pointing away from the wearer's ears.
- Leave a way to power it off without taking the mask apart.
