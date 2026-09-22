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
| 18650 cell + holder | Optional — see power below. Use a **protected** cell |
| TP4056 charger module | The version with DW01 protection |
| Buck-boost module (TPS63020 / TPS63060) | Battery to a stiff 3.3 V |
| 2 × 100 kΩ | Battery sense divider (optional) |
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
| 3V3 | VDD | Vin | everything runs on 3.3 V |
| GND | GND, **L/R** | GND | |

Two details that will cost you an evening if you miss them:

- **Tie the mic's L/R pin to GND.** That puts its data in the left I2S slot, which is
  what `MIC_SLOT_DEFAULT` expects. If you hear nothing, press `x` on the console to
  read the right slot instead.
- The amp's **GAIN** pin sets the analog gain: floating = 9 dB, to GND = 12 dB,
  100 kΩ to GND = 15 dB. On a 3.3 V rail, start at **15 dB** (100 kΩ to GND).

### Power — 3.3 V only, battery optional

**USB alone is enough.** Plug the devkit in and it runs: no cell, no divider, no
regulator. Battery monitoring is advisory, and an absent or unfitted sense divider
is detected and ignored rather than treated as a flat cell.

For a battery, everything runs from one 3.3 V rail:

```
18650 ──► TP4056 (+protection) ──┬──► buck-boost 3.3 V ──┬──► ESP32 3V3 pin
           (charge via USB)      │   (TPS63020 etc.)     └──► MAX98357A Vin
                                 │                            + 470-1000 µF
                                 └──► 100k ──┬── GPIO34             here
                                             │
                                            100k
                                             │
                                            GND
```

Four things matter here:

- **Use a buck-boost, not an LDO.** A cell runs 4.2 V down to 3.0 V, so a linear
  regulator drops out well before the cell is empty and the volume sags with it. A
  buck-boost holds 3.3 V from 4.2 V all the way down to ~2.5 V, so the mask stays
  equally loud until it stops.
- **Feed the 3V3 pin, never 5V/VIN.** The devkit's AMS1117 needs ~4.7 V in and will
  simply brown out on 3.3 V.
- **Do not let USB and the regulator both drive 3V3.** Two regulators fighting over
  the same rail is asking for trouble. Put a switch (or a Schottky from the
  buck-boost to 3V3) in that path, and disconnect the battery rail while flashing.
- **Bulk cap right at the amp's Vin**, 470–1000 µF plus a 0.1 µF, and star-wire both
  loads from the regulator output. The amp and the ESP32 now share a rail, so
  class-D bass transients land directly on the MCU supply — this cap is what keeps
  it from resetting.

At 3.3 V the MAX98357A makes roughly 1.2 W into 4 Ω rather than its 3.2 W at 5 V.
That is still plenty inside a mask, and 4 Ω (not 8 Ω) plus 15 dB gain gets most of
it back.

The firmware warns below 3.4 V and, after five consecutive low reads, shuts down
into deep sleep to protect the cell; the button wakes it again. Readings outside
2.5–4.5 V are treated as "no battery sense" and ignored. Set `BATTERY_MONITOR 0` in
`main/config.h` to compile the whole thing out.

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
b         read the battery sense pin (raw ADC + pin mV)
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
| Random reboots on loud bass | Bulk cap missing at the amp — it now shares the 3.3 V rail with the ESP32 |
| Quiet and getting quieter | Cell draining through an LDO instead of a buck-boost; press `b` |
| `battery N mV - shutting down` | Sense divider not fitted or mis-wired. Press `b` for the raw count; readings outside 2.5–4.5 V are ignored now, and five consecutive low reads are needed to shut down |

## Safety

- Use a protected 18650 and a charger module with protection. Never charge an
  unattended cell inside a mask.
- Class-D at 3 W a few centimetres from your ears is genuinely loud. Set `o` low
  first, and keep the speaker pointing away from the wearer's ears.
- Leave a way to power it off without taking the mask apart.
