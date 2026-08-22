/* Tape Echo 2 — Schwung audio_fx module for Ableton Move.
 *
 * A thin audio_fx_api_v2 shell around duskaudio::TapeEchoDSP, the
 * framework-free component-modeled three-head tape echo + spring reverb from
 * Tape Echo 2 by Dusk Audio (GPL-3.0). The vendored core in src/ported/ is
 * unmodified upstream; everything Move-specific lives in this file.
 *
 * Tempo sync follows the reference DPF shell: the selected Echo Rate detent
 * is mapped through the leading playback head's note table, converted to a
 * head-1 motor time at the host BPM, and CLAMPED (never octave-folded) to the
 * measured motor range. The core's motor-inertia smoother turns tempo changes
 * into tape-style glides.
 *
 * Realtime rules: process_block never allocates, never logs, never touches
 * the filesystem. Parameter setters are atomic stores; the core snapshots
 * them once per block.
 */

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../host/plugin_api_v1.h"
#include "../host/audio_fx_api_v2.h"
#include "../ported/core/TapeEchoDSP.hpp"   /* pulls in TapeEchoParams.hpp */
#include "te2_params.h"

static const host_api_v1_t *g_host = nullptr;

static void te2_log(const char *msg)
{
    if (g_host && g_host->log) g_host->log(msg);
}

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

struct te2_instance {
    duskaudio::TapeEchoDSP dsp;
    std::atomic<float> values[TE2_PARAM_COUNT];

    float bufL[MOVE_FRAMES_PER_BLOCK];
    float bufR[MOVE_FRAMES_PER_BLOCK];
    float rawL[MOVE_FRAMES_PER_BLOCK];   /* input kept for the shell-side dry */
    float rawR[MOVE_FRAMES_PER_BLOCK];

    /* Ping-pong stage. The core's wet bus is mono (one tape loop), so the
     * alternation is produced here rather than by cross-feeding the loop —
     * that would mean editing the vendored core. See te2_ping_pong(). */
    duskaudio::Biquad dryLowShelf[2], dryHighShelf[2];
    double ppPhase = 0.0;      /* 0..1 over one full L->R->L cycle */
    float  ppSign  = 0.0f;     /* smoothed square, -1..+1 */
    bool   shelvesReady = false;

    /* serve buffers (control thread only) */
    char chain_buf[8192];
    char state_buf[1536];
};

static float te2_clamp(const te2_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

static int te2_param_index(const char *key)
{
    for (int i = 0; i < TE2_PARAM_COUNT; i++)
        if (!strcmp(te2_params[i].key, key))
            return i;
    return -1;
}

/* Push one shell value into the DSP core. RATE is deliberately absent:
 * process_block owns the motor speed every block (knob or tempo sync). */
static void te2_push(te2_instance *inst, int idx, float v)
{
    duskaudio::TapeEchoDSP &d = inst->dsp;
    switch (idx) {
    case TE2_P_MODE:        d.setMode((int)(v + 0.5f) + 1);          break;
    case TE2_P_INTENSITY:   d.setIntensity(v);                       break;
    case TE2_P_ECHO_VOL:    d.setEchoLevel(v);                       break;
    case TE2_P_ECHO_PAN:    d.setEchoPan(v);                         break;
    case TE2_P_SEND:        d.setInputSend(v > 0.5f);                break;
    case TE2_P_INPUT:       d.setInputGain(v);                       break;
    case TE2_P_BASS:        d.setBass(v);                            break;
    case TE2_P_TREBLE:      d.setTreble(v);                          break;
    case TE2_P_WOW:         d.setWowFlutter(v);                      break;
    case TE2_P_AGE:         d.setTapeAge(v * 0.5f);                  break;
    case TE2_P_REVERB_VOL:  d.setReverbLevel(v);                     break;
    case TE2_P_REVERB_PAN:  d.setReverbPan(v);                       break;
    case TE2_P_MIX:         d.setMix(v);                             break;
    case TE2_P_OUTPUT_VOL:  d.setOutputVolume(v);                    break;
    case TE2_P_POWER:       d.setBypass(v < 0.5f);                   break;
    case TE2_P_DRY:         d.setDryLevel(v);                        break;
    default: break; /* RATE / SYNC / NOTE / PRESET are shell-level */
    }
}

static void te2_set_index(te2_instance *inst, int idx, float v)
{
    v = te2_clamp(&te2_params[idx], v);
    inst->values[idx].store(v, std::memory_order_relaxed);
    te2_push(inst, idx, v);
}

/* Factory programs, straight from the vendored kFactoryPresets. The stored
 * semantic division is converted to the physical detent through the preset
 * mode's leading head, exactly as the reference shell's loadProgram does. */
static void te2_apply_preset(te2_instance *inst, int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= kNumFactoryPresets) return;
    const TapeEchoPreset &p = kFactoryPresets[presetIdx];

    const int mode1to12 = (int)(p.v[kParamMode] + 0.5f);
    te2_set_index(inst, TE2_P_MODE,       (float)(mode1to12 - 1));
    te2_set_index(inst, TE2_P_RATE,       p.v[kParamRepeatRate]);
    te2_set_index(inst, TE2_P_INTENSITY,  p.v[kParamIntensity]);
    te2_set_index(inst, TE2_P_ECHO_VOL,   p.v[kParamEchoLevel]);
    te2_set_index(inst, TE2_P_REVERB_VOL, p.v[kParamReverbLevel]);
    te2_set_index(inst, TE2_P_BASS,       p.v[kParamBass]);
    te2_set_index(inst, TE2_P_TREBLE,     p.v[kParamTreble]);
    te2_set_index(inst, TE2_P_INPUT,      p.v[kParamInputGain]);
    te2_set_index(inst, TE2_P_WOW,        p.v[kParamWowFlutter]);
    te2_set_index(inst, TE2_P_DRY,        p.v[kParamDryLevel]);
    te2_set_index(inst, TE2_P_SYNC,       p.v[kParamTempoSync]);

    const int division = (int)(p.v[kParamSyncDivision] + 0.5f);
    const int detent = teSyncKnobPosForDivision(
        division, teLeadingHeadIndexForMode(mode1to12)) + 1;
    te2_set_index(inst, TE2_P_NOTE, (float)detent);

    const float age = teQuantizeTapeAge(p.v[kParamTapeAge]);
    te2_set_index(inst, TE2_P_AGE, age < 0.25f ? 0.0f : (age < 0.75f ? 1.0f : 2.0f));

    te2_set_index(inst, TE2_P_OUTPUT_VOL, p.outputVolume);
    te2_set_index(inst, TE2_P_ECHO_PAN,   p.echoPan);
    te2_set_index(inst, TE2_P_REVERB_PAN, p.reverbPan);
    te2_set_index(inst, TE2_P_SEND,       p.inputSend);
    te2_set_index(inst, TE2_P_MIX,        p.mix);

    inst->values[TE2_P_PRESET].store((float)presetIdx, std::memory_order_relaxed);
}

static int te2_json_number(const char *json, const char *key, float *out);

/* ------------------------------------------------------------------ */
/* TapeDelay (schwung-space-delay) preset compatibility                */
/*                                                                     */
/* That module saves its patch state as                                */
/*   {"time":400,"feedback":0.4,"mix":0.35,"tone":0.6,                 */
/*    "stereo_width":0,"division":"free","bpm":120}                    */
/* and its own set_param takes the same keys. We accept both spellings */
/* so an old patch, or anything replaying its parameters, lands on the */
/* nearest honest Tape Echo setting instead of being dropped.          */
/* ------------------------------------------------------------------ */

/* time -> the head that can actually reach it, plus the motor speed.
 * The transport only spans 69.33..177.354 ms on head 1; heads 2 and 3
 * multiply that by 1.91172 / 2.76118, so between them they cover roughly
 * 69..490 ms. Anything outside pins at the nearest end, which is what the
 * hardware does with an out-of-range request. */
static void te2_legacy_time(te2_instance *inst, float ms)
{
    int best = 0;
    float bestErr = 1e9f;
    float bestBase = 0.0f;
    for (int h = 0; h < 3; h++) {
        const float ratio  = duskaudio::TapeEchoDSP::kHeadRatio[h];
        const float offset = duskaudio::TapeEchoDSP::kHeadOffsetMs[h];
        float base = (ms - offset) / ratio;
        if (base < duskaudio::TapeEchoDSP::kMinDelayMs) base = duskaudio::TapeEchoDSP::kMinDelayMs;
        if (base > duskaudio::TapeEchoDSP::kMaxDelayMs) base = duskaudio::TapeEchoDSP::kMaxDelayMs;
        const float got = base * ratio + offset;
        const float err = got > ms ? got - ms : ms - got;
        if (err < bestErr) { bestErr = err; best = h; bestBase = base; }
    }
    te2_set_index(inst, TE2_P_MODE, (float)best);          /* H1 / H2 / H3 */
    te2_set_index(inst, TE2_P_RATE,
                  duskaudio::TapeEchoDSP::repeatRateForDelayMs(bestBase));
}

/* feedback -> intensity. TapeDelay's 0..1 is a loop gain of 0..0.95, which
 * always decays; Tape Echo self-oscillates past about 0.75. Scaling to that
 * threshold keeps a maxed-out old preset at the edge of runaway rather than
 * turning it into a siren on load. */
static void te2_legacy_feedback(te2_instance *inst, float v)
{
    te2_set_index(inst, TE2_P_INTENSITY, v * 0.75f);
}

/* tone -> treble. TapeDelay's tone is a one-pole lowpass swept 500 Hz..12 kHz;
 * the nearest control here is the echo-path treble shelf, centred at its
 * midpoint. */
static void te2_legacy_tone(te2_instance *inst, float v)
{
    te2_set_index(inst, TE2_P_TREBLE, v * 2.0f - 1.0f);
}

/* division -> tempo sync + the nearest physical Echo Rate detent for whichever
 * head is leading. "free" just turns sync off. */
static void te2_legacy_division(te2_instance *inst, const char *name)
{
    static const struct { const char *name; double beats; } kDiv[] = {
        { "1/1", 4.0 }, { "1/2", 2.0 }, { "1/2d", 3.0 }, { "1/4", 1.0 },
        { "1/4d", 1.5 }, { "1/4t", 2.0 / 3.0 }, { "1/8", 0.5 }, { "1/8d", 0.75 },
        { "1/8t", 1.0 / 3.0 }, { "1/16", 0.25 }, { "1/16t", 1.0 / 6.0 },
    };
    if (!strcmp(name, "free") || !strcmp(name, "0")) {
        te2_set_index(inst, TE2_P_SYNC, 0.0f);
        return;
    }
    double beats = 0.0;
    for (size_t i = 0; i < sizeof kDiv / sizeof kDiv[0]; i++)
        if (!strcmp(kDiv[i].name, name)) { beats = kDiv[i].beats; break; }
    if (beats <= 0.0) return;                       /* unknown spelling */

    const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
    const int head = teLeadingHeadIndexForMode(mode);
    int bestPos = 0;
    double bestErr = 1e9;
    for (int pos = 0; pos < kNumSyncKnobPositions; pos++) {
        const double b = kSyncDivisions[teDivisionForSyncKnobPos(pos, head)].beats;
        const double err = b > beats ? b - beats : beats - b;
        if (err < bestErr) { bestErr = err; bestPos = pos; }
    }
    te2_set_index(inst, TE2_P_SYNC, 1.0f);
    te2_set_index(inst, TE2_P_NOTE, (float)(bestPos + 1));
}

/* Pull a JSON string value out of a flat object. */
static int te2_json_string(const char *json, const char *k, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", k);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    return 0;
}

/* Returns 1 if the blob was a TapeDelay state and has been applied. */
static int te2_try_legacy_state(te2_instance *inst, const char *val)
{
    float v;
    /* "time" plus "feedback" is the signature; a Tape Echo blob has neither. */
    if (te2_json_number(val, "time", &v) != 0) return 0;
    float fb;
    if (te2_json_number(val, "feedback", &fb) != 0) return 0;

    te2_legacy_time(inst, v);
    te2_legacy_feedback(inst, fb);
    if (te2_json_number(val, "mix", &v) == 0)  te2_set_index(inst, TE2_P_MIX, v);
    if (te2_json_number(val, "tone", &v) == 0) te2_legacy_tone(inst, v);
    char div[24];
    if (te2_json_string(val, "division", div, sizeof div) == 0)
        te2_legacy_division(inst, div);
    /* stereo_width used to have no counterpart and was dropped. Now that the
     * ping-pong widener exists it maps directly, and any non-zero width arms
     * ping-pong — matching TapeDelay, where width IS what makes its two
     * cross-fed lines alternate. */
    if (te2_json_number(val, "stereo_width", &v) == 0) {
        te2_set_index(inst, TE2_P_WIDTH, v);
        te2_set_index(inst, TE2_P_PINGPONG, v > 0.0f ? 1.0f : 0.0f);
    }
    te2_log("tape-echo2: imported a TapeDelay preset");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ping-pong / stereo width                                            */
/*                                                                     */
/* Tape Echo has ONE mono tape loop, so there is no second delay line to
 * cross-feed the way a stereo ping-pong delay does — that lives inside the
 * feedback path, and the core is vendored unmodified. Instead the core is
 * asked for a wet-only bus (Mix = 1 makes its dry gain zero) and the
 * alternation is applied out here, then the dry is blended back using the
 * core's own mix law.
 *
 * Successive repeats arrive one head period apart, so panning the wet with a
 * square of period 2T puts repeat n on one side and repeat n+1 on the other:
 * that is what ping-pong is. It is exact while a single head is active. In a
 * multi-head mode the taps sit at 1.00 / 1.91 / 2.76 of the period and no
 * single square separates them, so the effect becomes stereo movement rather
 * than strict alternation — which is the honest limit of doing this outside
 * the loop, not a bug.
 *
 * With Ping Pong off, or Width at 0, none of this runs: the core mixes as it
 * always has and the output is bit-identical to a build without the feature.
 */
static void te2_prepare_shelves(te2_instance *inst, double fs)
{
    /* The direct monitor path the core applies to its dry signal — same
     * constants, so the blend below reconstructs it rather than approximating
     * it. (TapeEchoDSP::prepare, "direct monitor path".) */
    for (int c = 0; c < 2; c++) {
        inst->dryLowShelf[c].setCoeffs(duskaudio::Biquad::shelf(
            fs, 46.3316f, -6.17579f, 0.492074f, /*high=*/false));
        inst->dryHighShelf[c].setCoeffs(duskaudio::Biquad::shelf(
            fs, 13762.47f, -6.13329f, 0.492299f, /*high=*/true));
    }
    inst->shelvesReady = true;
}

/* Period between successive repeats of the leading head, in samples. */
static double te2_repeat_period(te2_instance *inst, float rate01, double fs)
{
    const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
    const int head = teLeadingHeadIndexForMode(mode);
    const double ms = (double)duskaudio::TapeEchoDSP::delayMsForRepeatRate(rate01)
                        * duskaudio::TapeEchoDSP::kHeadRatio[head]
                      + duskaudio::TapeEchoDSP::kHeadOffsetMs[head];
    return (ms > 1.0 ? ms : 1.0) * 0.001 * fs;
}

static void te2_ping_pong(te2_instance *inst, int n, float rate01, double fs)
{
    const float width = inst->values[TE2_P_WIDTH].load(std::memory_order_relaxed) * 0.01f;
    const float mix   = inst->values[TE2_P_MIX].load(std::memory_order_relaxed);
    const float dryLv = inst->values[TE2_P_DRY].load(std::memory_order_relaxed);
    const float outV  = inst->values[TE2_P_OUTPUT_VOL].load(std::memory_order_relaxed);
    const float drive = inst->values[TE2_P_INPUT].load(std::memory_order_relaxed);

    /* the core's own laws, so a blend here matches what it would have done */
    const float dryMixGain = mix < 0.5f ? 1.0f : 2.0f * (1.0f - mix);
    const float wetMixGain = mix > 0.5f ? 1.0f : 2.0f * mix;
    const float outputGain = std::pow(10.0f, 2.0f * outV - 1.0f);
    /* the core's front-panel input taper (private there; same curve, and the
     * loadtest proves the reconstruction matches the core's own mix) */
    const float inputGain  = drive * (1.29212f + 1.40165f * drive);
    const float dryGain    = outputGain * dryMixGain * dryLv * inputGain;

    const double period = 2.0 * te2_repeat_period(inst, rate01, fs);  /* L->R->L */
    const double step   = 1.0 / period;
    /* ~4 ms one-pole on the square keeps the hand-over click-free */
    const float  smooth = 1.0f - std::exp(-1.0f / (0.004f * (float)fs));

    for (int i = 0; i < n; i++) {
        inst->ppPhase += step;
        if (inst->ppPhase >= 1.0) inst->ppPhase -= 1.0;
        const float target = inst->ppPhase < 0.5 ? 1.0f : -1.0f;
        inst->ppSign += smooth * (target - inst->ppSign);

        const float s  = width * inst->ppSign;
        /* constant power across the swing: at s=0 both sides stay at unity,
         * which is exactly the centred wet the core would have produced. */
        const float comp = 1.0f / std::sqrt(1.0f + s * s);
        const float wl = inst->bufL[i] * (1.0f + s) * comp;
        const float wr = inst->bufR[i] * (1.0f - s) * comp;

        const float dl = inst->dryHighShelf[0].process(
                            inst->dryLowShelf[0].process(inst->rawL[i]));
        const float dr = inst->dryHighShelf[1].process(
                            inst->dryLowShelf[1].process(inst->rawR[i]));

        inst->bufL[i] = dryGain * dl + wetMixGain * wl;
        inst->bufR[i] = dryGain * dr + wetMixGain * wr;
    }
}

/* ------------------------------------------------------------------ */
/* v2 entry points                                                     */
/* ------------------------------------------------------------------ */

static void *te2_create_instance(const char * /*module_dir*/,
                                 const char * /*config_json*/)
{
    auto *inst = new (std::nothrow) te2_instance();
    if (!inst) return nullptr;

    const int sr     = g_host ? g_host->sample_rate     : MOVE_SAMPLE_RATE;
    const int frames = g_host ? g_host->frames_per_block : MOVE_FRAMES_PER_BLOCK;
    inst->dsp.prepare((double)sr,
                      frames > MOVE_FRAMES_PER_BLOCK ? frames : MOVE_FRAMES_PER_BLOCK);
    inst->dsp.reset();

    for (int i = 0; i < TE2_PARAM_COUNT; i++)
        te2_set_index(inst, i, te2_params[i].def);

    te2_log("tape-echo2: instance created");
    return inst;
}

static void te2_destroy_instance(void *instance)
{
    delete (te2_instance *)instance;
}

static void te2_process_block(void *instance, int16_t *audio_inout, int frames)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !audio_inout || frames <= 0) return;

    float effRate = 0.0f;

    /* Motor speed, once per block: tempo sync wins, otherwise the knob. */
    if (inst->values[TE2_P_SYNC].load(std::memory_order_relaxed) > 0.5f) {
        double bpm = (g_host && g_host->get_bpm) ? (double)g_host->get_bpm() : 120.0;
        const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
        const double ratio  = duskaudio::TapeEchoDSP::leadingHeadRatioForMode(mode);
        const double offset = duskaudio::TapeEchoDSP::leadingHeadOffsetMsForMode(mode);
        const int division = teDivisionForSyncKnobPos(
            (int)(inst->values[TE2_P_NOTE].load(std::memory_order_relaxed) + 0.5f) - 1,
            teLeadingHeadIndexForMode(mode));
        const double requestedHead1Ms = (syncDelayMs(bpm, division) - offset) / ratio;
        /* CLAMP, do not octave-fold — the hardware pins out-of-range notes at
         * the motor limit (see the reference shell for the measurements). */
        double clamped = requestedHead1Ms;
        if (clamped < (double)duskaudio::TapeEchoDSP::kMinDelayMs)
            clamped = (double)duskaudio::TapeEchoDSP::kMinDelayMs;
        if (clamped > (double)duskaudio::TapeEchoDSP::kMaxDelayMs)
            clamped = (double)duskaudio::TapeEchoDSP::kMaxDelayMs;
        effRate = duskaudio::TapeEchoDSP::repeatRateForDelayMs((float)clamped);
        inst->dsp.setRepeatRate(effRate);
    } else {
        effRate = inst->values[TE2_P_RATE].load(std::memory_order_relaxed);
        inst->dsp.setRepeatRate(effRate);
    }

    /* The stereo stage only engages when it has something to do. Otherwise the
     * core mixes exactly as it always has — no reconstructed dry, no extra
     * gain staging, bit-identical to a build without ping-pong. */
    const bool ppOn  = inst->values[TE2_P_PINGPONG].load(std::memory_order_relaxed) > 0.5f;
    const bool powOn = inst->values[TE2_P_POWER].load(std::memory_order_relaxed) > 0.5f;
    /* Width 0 still runs the blend so it stays exercised: with no swing the
     * reconstructed dry + centred wet must equal what the core would have
     * mixed itself, which the loadtest asserts sample by sample. */
    const bool wide = ppOn && powOn;

    inst->dsp.setMix(wide ? 1.0f    /* wet only; the blend happens out here */
                          : inst->values[TE2_P_MIX].load(std::memory_order_relaxed));
    if (wide && !inst->shelvesReady)
        te2_prepare_shelves(inst, g_host ? (double)g_host->sample_rate : (double)MOVE_SAMPLE_RATE);

    int16_t *p = audio_inout;
    while (frames > 0) {
        const int n = frames > MOVE_FRAMES_PER_BLOCK ? MOVE_FRAMES_PER_BLOCK : frames;

        for (int i = 0; i < n; i++) {
            inst->bufL[i] = (float)p[i * 2]     * (1.0f / 32768.0f);
            inst->bufR[i] = (float)p[i * 2 + 1] * (1.0f / 32768.0f);
        }
        if (wide) {
            memcpy(inst->rawL, inst->bufL, sizeof(float) * (size_t)n);
            memcpy(inst->rawR, inst->bufR, sizeof(float) * (size_t)n);
        }

        const float *ins[2] = { inst->bufL, inst->bufR };
        float *outs[2]      = { inst->bufL, inst->bufR };
        inst->dsp.processBlock(ins, outs, 2, n);
        if (wide) te2_ping_pong(inst, n, effRate,
                                g_host ? (double)g_host->sample_rate
                                       : (double)MOVE_SAMPLE_RATE);

        for (int i = 0; i < n; i++) {
            float l = inst->bufL[i] * 32767.0f;
            float r = inst->bufR[i] * 32767.0f;
            if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
            if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
            p[i * 2]     = (int16_t)l;
            p[i * 2 + 1] = (int16_t)r;
        }

        p += n * 2;
        frames -= n;
    }
}

/* ------------------------------------------------------------------ */
/* set / get                                                           */
/* ------------------------------------------------------------------ */

static int te2_enum_index(const te2_param_t *prm, const char *val)
{
    for (int i = 0; i < prm->n_options; i++)
        if (!strcmp(prm->options[i], val))
            return i;
    /* case-insensitive fallback */
    for (int i = 0; i < prm->n_options; i++) {
        const char *a = prm->options[i], *b = val;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

static int te2_json_number(const char *json, const char *key, float *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static void te2_set_param(void *instance, const char *key, const char *val)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !key || !val) return;

    /* the chain host may deliver keys with a component prefix */
    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "state")) {
        float v;
        if (te2_json_number(val, "te2", &v) != 0) {
            /* Not ours — it may still be a TapeDelay patch worth importing. */
            te2_try_legacy_state(inst, val);
            return;
        }
        for (int i = 0; i < TE2_PARAM_COUNT; i++) {
            if (i == TE2_P_PRESET) continue; /* a launcher, not state */
            if (te2_json_number(val, te2_params[i].key, &v) == 0)
                te2_set_index(inst, i, v);
        }
        return;
    }

    /* TapeDelay parameter names, for anything that replays them one at a time.
     * "mix" and "stereo_width" are spelled the same in both and need no alias
     * — stereo_width now drives the real ping-pong widener. */
    if (!strcmp(key, "time"))         { te2_legacy_time(inst, (float)atof(val)); return; }
    if (!strcmp(key, "feedback"))     { te2_legacy_feedback(inst, (float)atof(val)); return; }
    if (!strcmp(key, "tone"))         { te2_legacy_tone(inst, (float)atof(val)); return; }
    if (!strcmp(key, "division"))     { te2_legacy_division(inst, val); return; }

    const int idx = te2_param_index(key);
    if (idx < 0) return;
    const te2_param_t *prm = &te2_params[idx];

    float v;
    if (prm->type == TE2_ENUM) {
        const int oi = te2_enum_index(prm, val);
        v = oi >= 0 ? (float)oi : (float)atof(val);
    } else {
        v = (float)atof(val);
    }

    if (idx == TE2_P_PRESET) {
        te2_apply_preset(inst, (int)(te2_clamp(prm, v) + 0.5f));
        return;
    }
    te2_set_index(inst, idx, v);
}

static int te2_write_str(char *buf, int buf_len, const char *s)
{
    int n = (int)strlen(s);
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    return n;
}

/* The knobs the root page offers, in order. Move has 8 encoders, and the
 * chain UI shows exactly the "knobs" list — the rest stay reachable through
 * the full "params" array (and through movy's own pages). */
static const char *const te2_root_knobs[8] = {
    "mode", "repeat_rate", "intensity", "echo_volume",
    "reverb_volume", "mix", "tempo_sync", "echo_rate_note",
};

/* Writes the parameter array shared by chain_params and ui_hierarchy.
 * Returns bytes written, or -1 if it would not fit. */
static int te2_write_param_array(char *o, size_t cap)
{
    size_t w = 0;
    w += (size_t)snprintf(o + w, cap - w, "[");
    for (int i = 0; i < TE2_VISIBLE_PARAM_COUNT; i++) {
        const te2_param_t *p = &te2_params[i];
        if (i) w += (size_t)snprintf(o + w, cap - w, ",");
        w += (size_t)snprintf(o + w, cap - w,
                              "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"",
                              p->key, p->name,
                              p->type == TE2_ENUM ? "enum"
                              : p->type == TE2_INT ? "int" : "float");
        if (p->type == TE2_ENUM) {
            w += (size_t)snprintf(o + w, cap - w, ",\"options\":[");
            for (int j = 0; j < p->n_options; j++)
                w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"",
                                      j ? "," : "", p->options[j]);
            w += (size_t)snprintf(o + w, cap - w, "],\"default\":\"%s\"}",
                                  p->options[(int)(p->def + 0.5f)]);
        } else if (p->type == TE2_INT) {
            w += (size_t)snprintf(o + w, cap - w,
                                  ",\"min\":%d,\"max\":%d,\"default\":%d}",
                                  (int)p->min, (int)p->max, (int)p->def);
        } else {
            w += (size_t)snprintf(o + w, cap - w,
                                  ",\"min\":%g,\"max\":%g,\"default\":%g}",
                                  (double)p->min, (double)p->max, (double)p->def);
        }
        if (w >= cap - 2) return -1;
    }
    w += (size_t)snprintf(o + w, cap - w, "]");
    if (w >= cap) return -1;
    return (int)w;
}

static int te2_serve_chain_params(te2_instance *inst, char *buf, int buf_len)
{
    if (te2_write_param_array(inst->chain_buf, sizeof inst->chain_buf) < 0)
        return -1;
    return te2_write_str(buf, buf_len, inst->chain_buf);
}

/* Full hierarchy: EVERY parameter, with the 8 encoder assignments up front.
 * Built from the same table as chain_params so the two can never disagree —
 * an earlier hand-written version advertised only 4 params and hid the other
 * fifteen from anything that reads the hierarchy. */
static int te2_serve_ui_hierarchy(te2_instance *inst, char *buf, int buf_len)
{
    char *o = inst->chain_buf;
    const size_t cap = sizeof inst->chain_buf;
    size_t w = (size_t)snprintf(o, cap,
        "{\"modes\":null,\"levels\":{\"root\":{\"children\":null,\"knobs\":[");
    for (int i = 0; i < 8; i++)
        w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"", i ? "," : "", te2_root_knobs[i]);
    w += (size_t)snprintf(o + w, cap - w, "],\"params\":");
    if (w >= cap - 2) return -1;
    const int n = te2_write_param_array(o + w, cap - w);
    if (n < 0) return -1;
    w += (size_t)n;
    w += (size_t)snprintf(o + w, cap - w, "}}}");
    if (w >= cap) return -1;
    return te2_write_str(buf, buf_len, o);
}

static int te2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "name"))
        return te2_write_str(buf, buf_len, "Tape Echo 2");

    if (!strcmp(key, "chain_params"))
        return te2_serve_chain_params(inst, buf, buf_len);

    if (!strcmp(key, "state")) {
        char *o = inst->state_buf;
        const size_t cap = sizeof inst->state_buf;
        size_t w = (size_t)snprintf(o, cap, "{\"te2\":1");
        for (int i = 0; i < TE2_PARAM_COUNT; i++) {
            if (i == TE2_P_PRESET) continue;
            w += (size_t)snprintf(o + w, cap - w, ",\"%s\":%.6g",
                                  te2_params[i].key,
                                  (double)inst->values[i].load(std::memory_order_relaxed));
            if (w >= cap - 32) return -1;
        }
        w += (size_t)snprintf(o + w, cap - w, "}");
        return te2_write_str(buf, buf_len, o);
    }

    if (!strcmp(key, "ui_hierarchy"))
        return te2_serve_ui_hierarchy(inst, buf, buf_len);

    /* record-path meters (0..3), for UI polling */
    if (!strcmp(key, "out_level"))
        return snprintf(buf, buf_len, "%.3f", (double)inst->dsp.getRecordVuLevel());
    if (!strcmp(key, "peak_level"))
        return snprintf(buf, buf_len, "%.3f", (double)inst->dsp.getRecordPeakLevel());

    /* the note name behind the current Echo Rate detent (leading-head table) */
    if (!strcmp(key, "echo_note_name")) {
        const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
        const int division = teDivisionForSyncKnobPos(
            (int)(inst->values[TE2_P_NOTE].load(std::memory_order_relaxed) + 0.5f) - 1,
            teLeadingHeadIndexForMode(mode));
        return te2_write_str(buf, buf_len, kSyncDivisions[division].name);
    }

    const int idx = te2_param_index(key);
    if (idx < 0) return -1;
    const te2_param_t *prm = &te2_params[idx];
    const float v = inst->values[idx].load(std::memory_order_relaxed);

    if (prm->type == TE2_ENUM) {
        int oi = (int)(v + 0.5f);
        if (oi < 0) oi = 0;
        if (oi >= prm->n_options) oi = prm->n_options - 1;
        return te2_write_str(buf, buf_len, prm->options[oi]);
    }
    if (prm->type == TE2_INT)
        return snprintf(buf, buf_len, "%d", (int)(v + (v >= 0 ? 0.5f : -0.5f)));
    return snprintf(buf, buf_len, "%.4f", (double)v);
}

/* ------------------------------------------------------------------ */

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof g_fx_api_v2);
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = te2_create_instance;
    g_fx_api_v2.destroy_instance = te2_destroy_instance;
    g_fx_api_v2.process_block    = te2_process_block;
    g_fx_api_v2.set_param        = te2_set_param;
    g_fx_api_v2.get_param        = te2_get_param;
    g_fx_api_v2.on_midi          = nullptr;

    te2_log("tape-echo2: Tape Echo 2 (Dusk Audio) audio_fx v2 initialized");
    return &g_fx_api_v2;
}
