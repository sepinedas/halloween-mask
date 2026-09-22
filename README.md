# Halloween demon-voice mask

Real-time voice changer for an ESP-WROOM-32: an **ICS-43434** I2S MEMS mic goes in,
a **MAX98357A** I2S class-D amp comes out, and in between the ESP32 pitches your
voice down, drives it into soft clipping and drops it in a crypt — or takes it up,
for a woman or a squirrel. Runs off a single **18650** cell, or just USB.
Five presets, cycled with one button.

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

Breakouts silkscreen these pins differently from the datasheets. Both names are
given below — **match the silkscreen on your board**, not the datasheet column.

| ESP32 | ICS-43434 (silkscreen / datasheet) | MAX98357A | Other |
| --- | --- | --- | --- |
| GPIO26 | `BCLK` / SCK | BCLK | |
| GPIO25 | `LRCL` / WS | LRC | |
| GPIO33 | `DOUT` / SD | | mic → ESP32 |
| GPIO22 | | DIN | ESP32 → amp |
| GPIO21 | | SD_MODE | high = on, low = shutdown |
| GPIO4 | | | button to GND (internal pull-up) |
| GPIO2 | | | on-board LED |
| GPIO34 | | | battery divider midpoint |
| 3V3 | `3V` / VDD | Vin | everything runs on 3.3 V |
| GND | `GND`, and **`SEL`** / L/R | GND | see below |

Three details that will cost you an evening if you miss them:

- **`SEL` is not the data pin.** The datasheet calls the data output `SD`, but the
  breakout silkscreens it `DOUT`; `SEL` is the channel-select pin. Wiring `SEL` to
  GPIO33 and leaving `DOUT` unconnected gives a mask where everything works except
  the microphone, and every voltage you measure looks correct.
- **Tie `SEL` (L/R) to GND.** Not floating — tied. That puts the mic's data in the
  left I2S slot, which is what `MIC_SLOT_DEFAULT` expects. If the meter shows the
  level on the right slot instead, press `x` on the console or flip that define.
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
| 2 | Woman | +5 semitones with a body lift — see the caveat below |
| 3 | Squirrel | A full octave up, clean and bright. Chipmunk, not demon |
| 4 | Clean | No shift, no effects — use this to test wiring |

**Serial console** at 115200 — tune by ear without reflashing, then copy the numbers
you like into `main/presets.h`. Press `h` for the list:

```
p -9      pitch voice 1, semitones      d 4.0    drive
P -12     pitch voice 2                 f 30     ring mod frequency
M 0.5     voice 2 mix                   r 0.3    ring mod mix
y 0.1     dry blend                     v 0.2    reverb mix
c 3500    tone lowpass, Hz              g 14     input gain
o 0.9     output gain                   t 0.014  gate threshold
s         stats (CPU load, levels)      l        live level meter
x         swap mic I2S slot             m        mute
b         read the battery sense pin (raw ADC + pin mV)
T         test tone on/off (bypasses mic and DSP)
B 4       body lift at 600 Hz, dB
```

### Getting it loud

At 3.3 V into 4 Ω the amp has about 1.2 W to give, so it is worth not wasting any
of it. In rough order of how much each one buys you:

| Lever | Gain | Notes |
| --- | --- | --- |
| **Seal the speaker into a baffle** | up to +6 dB | The biggest and cheapest win. An unbaffled small speaker cancels itself front-to-back and loses most of its midrange. Cut a snug hole, no gaps around the rim |
| **`GAIN` pin → 100 kΩ to GND** | +6 dB | 15 dB instead of the 9 dB you get leaving it floating |
| **4 Ω speaker, not 8 Ω** | +3 dB | Twice the power for the same voltage |
| **Console `g`** | varies | Input gain. Raise until `out` on the meter peaks near 0.8 |
| **Console `o`** | up to +1.5 dB | Output gain, presets ship at 0.8–0.9, maximum 1.0 |
| **Console `d`** | varies | More drive is now louder as well as dirtier |
| **5 V rail** | +4 dB | 3.2 W instead of 1.2 W — but that is the 3.3 V-only design decision, reversed |

Two cautions. Raising `g` also raises the noise floor and makes feedback more
likely, and because the gate sits *after* the input gain you should raise `t` in
the same proportion or it will start opening on room noise. And a mask puts the
speaker centimetres from the mic: if it howls, back off `o`, raise `t`, and point
the speaker away from the mic before reaching for more gain.

---

## How it works

```
mic ─► gain ─► DC block ─► highpass ─► gate ─► anti-alias ─┬─► pitch shift 1 ─┐
                                        (only pitching up) ├─► pitch shift 2 ─┤
                                                           └─── dry ──────────┴─►
       ─► ring mod ─► soft clip ─► tone lowpass ─► reverb ─► speaker HP ─►
       ─► limiter ─► amp
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

Pitching *up* needs one thing pitching down does not. Reading the delay line at 2×
speed maps input above 8 kHz past the 16 kHz Nyquist, where it folds back into the
audible band — sibilants turn metallic. So any preset with a positive pitch engages
a 6th-order Butterworth lowpass on the input, set to `0.45 · fs / ratio`. Measured
on a 10 kHz tone shifted an octave up, that drops the fold-back by 29 dB; a single
biquad only managed 10 dB, which is why it is a cascade.

Pitching *down* hits a hardware wall instead. Deep Demon sits 12 semitones below
your voice, putting a male speaker near 55 Hz, and a 40 mm speaker will not
reproduce that. The heavy drive on the deep presets is doing real work: it
generates harmonics the speaker *can* render, and the ear reconstructs the
missing fundamental from them. The output is then highpassed at `SPEAKER_HP_HZ`
(120 Hz) so the amp does not waste its ~1.2 W moving the cone at frequencies
nobody will hear — measured on a 17-semitone shift at 15 dB removed below 90 Hz
for 0.4 dB lost above 250 Hz.

### Why the Woman preset is a compromise

This shifter **resamples**, so pitch and formants move together. That is exactly
right for Squirrel — a chipmunk *is* a small resonant cavity — but wrong for a
woman. Male F0 runs 85–155 Hz against a female 165–255 Hz, which argues for about
+7 semitones; but a woman's vocal tract is only ~15% shorter than a man's, not
50%, so her formants sit only ~15–20% higher. Shift +7 and the formants rise 50%
too, and the result reads as a *child*.

Separating the two needs LPC or phase-vocoder formant shifting, which is a lot of
machinery for a mask. So the preset compromises: **+5 semitones** rather than +7,
and a broad **+4 dB lift at 600 Hz** (`bodyDb`, or `B` on the console) to put back
some of the chest that an upward shift thins out. It reads as a lighter, higher
voice rather than a convincing woman — worth knowing before you judge it. Tune it
live with `p` and `B` if your voice sits somewhere else.

The waveshaper deliberately has **no make-up attenuation**. It used to scale its
output by `1/(0.5 + 0.5·drive)` to hold loudness steady as drive rose, which had
the effect of throwing away 10–15 dB: measured across the presets, output peaked
between −10 and −15 dBFS and the limiter never engaged once. Since `softClip`
already bounds its own output to ±1, that attenuation bought nothing. Removing it
and raising the default input gain to 14 recovered 7–14 dB depending on the
preset, and the limiter now does the job it was there for.

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

## Bring-up order

For the first 20 s after reset the firmware prints a level line every second, with
no console input needed (`STARTUP_METER_SEC`):

```
in 0.0000  out 0.0000  L[-2..3] R[0..0] using L  cpu 9.8%  Demon
```

`L` and `R` are the raw 24-bit extremes on each I2S slot, straight off the bus
before any gain. A live mic dithers by a few counts even in a silent room.

1. **Press `T`** for the test tone. It bypasses the microphone and the whole DSP
   chain, so it splits the board in half.
   - *Silent* → the fault is the amp, its supply or the speaker. Check 3.3 V at the
     amp's Vin, GPIO21 high (~3.3 V) after boot, and the speaker: the MAX98357A
     output is **bridge-tied**, so the speaker floats across `+` and `−`. Grounding
     either terminal gives silence.
   - *Audible* → the whole output path works, including the I2S TX and clocks.
2. **Check the raw slots** while talking.
   - `L[0..0] R[0..0]` → no data on DIN at all. Check `DOUT` really goes to GPIO33
     (it is not the pin marked `SEL`), that the mic has power and a ground return,
     and that `BCLK`/`LRCL` reach the mic breakout.
   - One side flat, the other moving → set `MIC_SLOT_DEFAULT` to match, or press `x`.
   - Both moving → the mic is fine; press `4` for the clean preset and talk.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| No sound on the `T` test tone | Amp unpowered, SD_MODE low, or a speaker terminal shorted to GND (the output is bridge-tied — the speaker must float) |
| Console prints but ignores keypresses | Fixed: `uart_param_config` must run *before* `uart_driver_install`, or RX stays dead |
| Silence, `in` is 0.000, one raw slot moving | Mic is in the other I2S slot — press `x` or flip `MIC_SLOT_DEFAULT` |
| Silence, both raw slots read `[0..0]` | No data on DIN: `DOUT` not wired to GPIO33 (`SEL` is a different pin), no ground return, or clocks not reaching the mic |
| Squirrel sounds harsh or metallic on 's' sounds | Input anti-alias filter disabled or mis-set; it should engage automatically for any preset pitched up |
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
