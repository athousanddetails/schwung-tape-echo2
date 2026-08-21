# Tape Echo 2 for Ableton Move (Schwung)

A Schwung `audio_fx` module port of **[Tape Echo 2 by Dusk Audio](https://github.com/dusk-audio/dusk-audio-plugins/tree/main/plugins/tape-echo)** —
a component-modeled vintage three-head tape echo with spring reverb: 12
head/reverb modes, in-loop record EQ and tape saturation (so repeats darken
and compress into stable self-oscillation), wow & flutter, tape age
(New/Used/Old cartridge states), and Galaxy-style tempo sync.

**All DSP credit goes to Dusk Audio** — the vendored core in `src/ported/`
is unmodified upstream (GPL-3.0, see `LICENSE`). Move port: athousanddetails.

## Module layout on device

```
/data/UserData/schwung/modules/audio_fx/tape-echo2/
├── tape-echo2.so      # audio_fx_api_v2 plugin (move_audio_fx_init_v2)
│                      #   MUST be <id>.so — see note below
├── module.json
├── movy_config.json   # curated Movy knob banks (Echo / Tape)
├── ui_chain.js        # chain-mode parameter editor with record VU
├── help.json          # on-device help text, one section per control
└── web_ui.html        # browser panel (see Browser UI below)
```

**The library filename is load-bearing.** The chain host builds an FX path as
`modules/audio_fx/<id>/<id>.so` literally (`chain_host.c`) and never reads the
`"dsp"` field from `module.json` — only *sound_generators* are loaded as
`dsp.so`. Shipping `dsp.so` here makes the module appear in the FX picker (that
list is built from `module.json`) and then silently fail to load.
`tools/check_config.py` now enforces the name across module.json, CMake, and
both scripts.

## Controls — 14 parameters, 2 pages

| Page | Knob 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|--------|---|---|---|---|---|---|---|
| Echo | MODE | RATE | INTS | EVOL | RVOL | MIX | SYNC | NOTE |
| Tape | DRIV | BASS | TREB | WOW | AGE | SEND | – | – |

Page 1 is also the `ui_hierarchy` root, so those eight are the Move encoder
assignments in the native chain UI. `ui_chain.js` lists the same 14, four rows
at a time.

**Deliberately not published as knobs** — still accepted by `set_param` and
still saved/restored in `state`, just not on a page: `output_volume`,
`echo_pan`, `reverb_pan`, `power`, `preset`, `dry_level`. Output trim, panning
and bypass duplicate what the Move mixer and the chain slot already do, and
they sit at unity/center/on by default. Factory programs remain loadable with
`set_param("preset", "Dub Throw")`.

`Mode` names are compact on purpose — **Movy truncates an enum option to 5
characters** on the knob readout (`store.ts formatValue`), so the twelve modes
are `H1 H2 H3 H2+3 H1+R H2+R H3+R H12+R H23+R H13+R H123R Rev` (digits are the
active playback heads, `R` is the spring). The descriptive names the reference
plugin uses (`Heads 2+3 + Reverb`) all collapsed to `"Head "` on screen.
`tools/check_config.py` fails the build if any enum loses distinctness there.

Movy commits enums **by name**, and the DSP's `set_param` matches by name with
an index fallback; the loadtest round-trips all twelve.

Notes preserved from the reference:

- **Tempo sync** maps the 1–11 Echo Rate detent through the *leading playback
  head's* note table (changing Mode reassigns the detent's note, as on
  Galaxy), converts it to head-1 motor time at the host BPM, and **clamps**
  to the physical motor range (69.3–177.4 ms) — out-of-range notes pin at the
  endpoint, they are never octave-folded. Tempo changes glide like tape.
- **Intensity** above ~0.75 self-oscillates. It gets loud, as the hardware does.
- **Power** off is a click-free, sample-exact clean passthrough.
- **Mix** keeps the 50 % point at the classic parallel dry+wet calibration;
  0 is dry-only, 1 is wet-only.
- Hidden compatibility param `dry_level` is accepted via `set_param` and
  round-trips in state, but is not exposed on knobs.

## Browser UI

`web_ui.html` is a VST-style panel — knobs, switches, mode selector and a
record meter — that talks to the running module over schwung-manager's
remote-ui WebSocket. Open it directly:

```
http://move.local:7700/api/remote-ui/module-assets/tape-echo2/web_ui.html
```

Pick the track holding the module; it finds the FX slot from `slot_info`,
seeds every control from `chain_params`, and follows hardware knob moves live.
Drag a knob vertically (hold Shift for fine, double-click to reset).

**Why standalone rather than embedded:** schwung-manager only auto-loads a
module's `web_ui.html` for the **synth** component — `remote_ui.go` guards
every call site with `comp == "synth"`, and `renderCustomUI` replaces the whole
slot view, so an audio FX can never get an iframe without a manager change. The
page therefore uses `schwungRemote` when it is embedded and falls back to the
same WebSocket the manager itself speaks when it is not, which works on stock
Schwung today.

## Build

```bash
./scripts/build.sh        # canonical: rsync to VPS, Docker ubuntu:22.04 cross-build
./scripts/deploy.sh       # atomic install to move.local + loadtest binary
```

Local fallback without the VPS (used for the initial port): `zig c++`
cross-compiles directly from macOS —

```bash
zig c++ -target aarch64-linux-gnu.2.35 -O3 -std=c++17 -fPIC -shared -Wl,--strip-all \
  src/dsp/tape_echo_plugin.cpp src/ported/core/TapeEchoDSP.cpp \
  -Isrc -Isrc/host -Isrc/ported/core -Isrc/ported/shared-dpf/dsp -o build/tape-echo2.so
```

The result depends only on `libc`/`libm` (glibc symbols ≤ 2.34; the Move
ships 2.35) with libc++ statically linked.

## Tests

- `tools/check_config.py` — contract gate (runs first in the build):
  `movy_config.json`, `module.json` `chain_params`, `src/dsp/te2_params.h`, and
  `ui_chain.js` must describe the same surface; rows must be exactly 8 slots;
  short labels ≤ 5 chars; enum options must stay distinct after Movy's 5-char
  truncation.
- `tests/test_dsp.cpp` — native golden checks on the vendored core: motor
  rate↔delay inversion, sync math (1/8 @ 120 BPM = 250 ms), silence in →
  silence out, and the head-1 impulse echo landing at the modeled delay.
- `tools/loadtest.cpp` — dlopens `tape-echo2.so` exactly like the chain host:
  API surface, enum-by-name and float round trips, all 12 mode names,
  chain_params/state JSON, a drift gate proving `module.json` embeds exactly
  what the plugin serves, factory preset recall, audible echo, bypass
  passthrough, realtime factor.
- `tools/movy_layout.mjs` — boots the **real Movy model** with this module in
  an FX slot and prints the pages as they render on device, failing if Movy
  does not read our config or a param is unreachable. Needs a Movy checkout
  with `npm install && npm run build:browser`:

  ```bash
  node tools/movy_layout.mjs /path/to/movy
  ```

Measured on the Move (Cortex-A72), worst-case preset (all heads + spring):
**0.39 ms per 128-frame block ≈ 13 % of one core.**

## Release

Tag `vX.Y.Z` and push — `.github/workflows/release.yml` cross-builds in Docker,
attaches `dist/tape-echo2-module.tar.gz` to the GitHub release, and rewrites
`release.json` on `main` so the Schwung module catalog picks up the new version.

## License

GPL-3.0, inherited from Dusk Audio's upstream. `src/ported/` is verbatim
upstream code; the Schwung shell (`src/dsp/`, `src/ui_chain.js`, configs)
is this port's contribution, same license.
