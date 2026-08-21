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
        inst->dsp.setRepeatRate(
            duskaudio::TapeEchoDSP::repeatRateForDelayMs((float)clamped));
    } else {
        inst->dsp.setRepeatRate(
            inst->values[TE2_P_RATE].load(std::memory_order_relaxed));
    }

    int16_t *p = audio_inout;
    while (frames > 0) {
        const int n = frames > MOVE_FRAMES_PER_BLOCK ? MOVE_FRAMES_PER_BLOCK : frames;

        for (int i = 0; i < n; i++) {
            inst->bufL[i] = (float)p[i * 2]     * (1.0f / 32768.0f);
            inst->bufR[i] = (float)p[i * 2 + 1] * (1.0f / 32768.0f);
        }

        const float *ins[2] = { inst->bufL, inst->bufR };
        float *outs[2]      = { inst->bufL, inst->bufR };
        inst->dsp.processBlock(ins, outs, 2, n);

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
        if (te2_json_number(val, "te2", &v) != 0) return; /* not our blob */
        for (int i = 0; i < TE2_PARAM_COUNT; i++) {
            if (i == TE2_P_PRESET) continue; /* a launcher, not state */
            if (te2_json_number(val, te2_params[i].key, &v) == 0)
                te2_set_index(inst, i, v);
        }
        return;
    }

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
