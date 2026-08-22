# Tape Echo 2 — Tape Echo and Spring Reverb for Ableton Move

A component-modelled vintage three-head tape echo with spring reverb for
[Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Twelve
head and reverb combinations, record EQ and tape saturation inside the
feedback loop so every repeat darkens and compresses, wow and flutter, tape
age, and tempo sync with the reference machine's leading-head note tables.

![Tape Echo 2 — page 1](docs/page_1.png)
![Tape Echo 2 — page 2](docs/page_2.png)

## Controls

| Page | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| **Echo** | Mode | Rate | Intensity | Echo Vol | Reverb Vol | Mix | Tempo Sync | Rate Note |
| **Tape** | Drive | Bass | Treble | Wow/Flutter | Tape Age | Input Send | Ping Pong | Width |

Modes are `H1 H2 H3 H2+3 H1+R H2+R H3+R H12+R H23+R H13+R H123R Rev` — digits
are the live playback heads, `R` is the spring tank.

Intensity above ~75% self-oscillates. Input Send is the dub switch: off stops
feeding the tape while existing repeats wash out. Bass and Treble are on the
echo path only.

Ping Pong alternates successive repeats left and right, Width sets how far it
swings. Each head alternates on its own delay, so the multi-head modes separate
too. With it off the echo bus is mono and the output is bit-identical to Dusk
Audio's original — the build checks that against upstream on every run.

Tape Echo 2 reads the older [TapeDelay](https://github.com/charlesvestal/schwung-space-delay)
module's settings: the stored delay time picks the playback head that can reach
it, and feedback, mix, tone, note division and stereo width carry across. A
patch names the module it wants, though, so point it at this one first:

```bash
python3 tools/convert_tapedelay_patch.py MyPatch.json
```

Works with [Movy](https://github.com/DimaDake/schwung-movy) — a
`movy_config.json` ships with the module.

## Remote panel

A tape-deck style editor in the browser: draggable knobs, mode and rate-note
selectors, tempo sync and input send switches, and a record meter. Open it
while the module is loaded in an FX slot:

```
move.local:7700/api/remote-ui/module-assets/tape-echo2/web_ui.html
```

![Tape Echo 2 remote panel](docs/remote-ui.png)

## Install

Via the Schwung Module Store, or manually: copy `dist/tape-echo2/` to
`/data/UserData/schwung/modules/audio_fx/tape-echo2/` on the device.

## Building

Requires Docker (cross-compiles for the Move's ARM64, pinned to glibc 2.35):

```bash
./scripts/build.sh               # builds build/tape-echo2.so + dist/tape-echo2-module.tar.gz
./scripts/deploy.sh <host>       # safe deploy (atomic rename, never over a live .so)
```

## Credits

- **[Tape Echo 2](https://github.com/dusk-audio/dusk-audio-plugins/tree/main/plugins/tape-echo)**
  by **Dusk Audio** (GPL-3.0) — the DSP, and all of the modelling credit.
  The core in `src/ported/` carries one Schwung-specific change, marked in the
  files: a per-head ping-pong stage on the echo output tap. Switch Ping Pong
  off and it is upstream sample for sample, which the build verifies against a
  fresh checkout of the original.
- Move port by athousanddetails.

GPL-3.0, inherited from upstream.
