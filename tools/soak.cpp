/* te2_soak — overnight-grade abuse of the built module, on the device.
 *
 *   ./te2_soak ./tape-echo2.so [minutes]
 *
 * The loadtest proves the thing works. This one tries to break it, and cares
 * about the numbers the loadtest averages away:
 *
 *   - WORST-CASE block time, not the mean. A realtime budget is blown by the
 *     one slow block, not by the average one.
 *   - NaN / Inf anywhere in the output. A feedback loop with a bad parameter
 *     combination produces them, and once one is on the tape it never leaves.
 *   - Denormal stalls: silence after a loud tail is where they appear, and
 *     they show up as a block-time spike rather than as wrong audio.
 *   - Parameter fuzz WHILE rendering, which is what actually happens on the
 *     device (set_param runs on the SPI callback between blocks). The macros
 *     make this worth doing: one write fans out to several members, so a
 *     macro can reach a combination no single knob can.
 *   - State round trip under fuzz: whatever the fuzzer reaches must survive
 *     get_param("state") -> a fresh instance -> set_param("state").
 *
 * Deliberately offline: it dlopens the module and renders into buffers.
 * Nothing reaches the DAC.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>

#include "../src/host/plugin_api_v1.h"
#include "../src/host/audio_fx_api_v2.h"

static void host_log(const char *) {}
static float host_bpm(void) { return 120.0f; }

/* xorshift: deterministic, so a failure can be reproduced from the seed */
static uint32_t rngState = 0x13579bdfu;
static uint32_t rnd(void)
{
    uint32_t x = rngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rngState = x;
}
static double rnd01(void) { return (double)(rnd() >> 8) / 16777216.0; }

struct KeyRange { const char *key; double lo, hi; bool isEnum; int nOpt; };
/* every published key, with its real range */
static KeyRange KEYS[] = {
    { "mode",            0, 11, true, 12 }, { "repeat_rate",   0, 1,   false, 0 },
    { "intensity",       0, 1,  false, 0 }, { "echo_volume",   0, 1,   false, 0 },
    { "reverb_volume",   0, 1,  false, 0 }, { "mix",           0, 1,   false, 0 },
    { "tempo_sync",      0, 1,  true, 2  }, { "echo_rate_note",1, 11,  false, 0 },
    { "input_volume",    0, 1,  false, 0 }, { "bass",         -1, 1,   false, 0 },
    { "treble",         -1, 1,  false, 0 }, { "wow_flutter",   0, 1,   false, 0 },
    { "tape_age",        0, 2,  true, 3  }, { "input_send",    0, 1,   true, 2 },
    { "ping_pong",       0, 1,  true, 2  }, { "stereo_width",  0, 100, false, 0 },
    { "time_ms",        69, 490, false, 0}, { "tone_tilt",    -1, 1,   false, 0 },
    { "tape_wear",       0, 1,  false, 0 },
};
static const int NKEYS = (int)(sizeof KEYS / sizeof KEYS[0]);

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "./tape-echo2.so";
    const double minutes = argc > 2 ? atof(argv[2]) : 5.0;

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) { printf("no init symbol\n"); return 1; }

    host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = 1; host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log; host.get_bpm = host_bpm;
    audio_fx_api_v2_t *fx = init(&host);

    void *inst = fx->create_instance(".", nullptr);
    if (!inst) { printf("create_instance failed\n"); return 1; }

    static int16_t io[MOVE_FRAMES_PER_BLOCK * 2];
    const double budgetMs = 1000.0 * MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE;

    double worstMs = 0, totalMs = 0;
    long blocks = 0, over = 0, bad = 0, writes = 0, stateFails = 0;
    long railRun = 0, worstRailRun = 0;
    int worstAtBlock = -1;
    char worstKey[64] = "-";
    char lastKey[64] = "-";

    const double endAt = (double)clock() / CLOCKS_PER_SEC + minutes * 60.0;
    /* phases: loud material, then silence (denormals), then fuzz */
    while ((double)clock() / CLOCKS_PER_SEC < endAt) {
        const int phase = (int)(blocks / 400) % 3;

        /* fuzz a parameter roughly every 8 blocks, as the device would */
        if ((blocks % 8) == 0) {
            const KeyRange &k = KEYS[rnd() % (uint32_t)NKEYS];
            char v[32];
            if (k.isEnum) snprintf(v, sizeof v, "%d", (int)(rnd() % (uint32_t)k.nOpt));
            else          snprintf(v, sizeof v, "%.4f", k.lo + rnd01() * (k.hi - k.lo));
            fx->set_param(inst, k.key, v);
            snprintf(lastKey, sizeof lastKey, "%s=%s", k.key, v);
            writes++;
        }

        memset(io, 0, sizeof io);
        if (phase == 0) {
            for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
                const double t = (double)(blocks * MOVE_FRAMES_PER_BLOCK + i);
                const double s = 0.3 * sin(2.0 * M_PI * 220.0 * t / MOVE_SAMPLE_RATE);
                io[i*2] = io[i*2+1] = (int16_t)(s * 32000);
            }
        } else if (phase == 2 && (blocks % 97) == 0) {
            io[0] = io[1] = 32000;              /* impulses into the tail */
        }
        /* phase 1 = silence: where denormals bite */

        const clock_t t0 = clock();
        fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
        const double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;

        totalMs += ms;
        if (ms > worstMs) { worstMs = ms; worstAtBlock = (int)blocks; strncpy(worstKey, lastKey, sizeof worstKey - 1); }
        if (ms > budgetMs) over++;

        /* Railing only means something during the SILENT phase. With signal in
         * and intensity fuzzed up to 1.0 this engine is supposed to run away —
         * that is the instrument, not a defect. A rail that persists through
         * silence is the signature of a NaN on the tape, which never leaves. */
        bool railed = false;
        for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
            if (io[i] == 32767 || io[i] == -32768) { railed = true; break; }
        if (phase == 1 && railed) {
            bad++;
            if (++railRun > worstRailRun) worstRailRun = railRun;
        } else if (!railed) {
            railRun = 0;
        }
        blocks++;

        /* every ~10 s, prove the state the fuzzer reached round-trips */
        if ((blocks % 3400) == 0) {
            static char st[8192];
            if (fx->get_param(inst, "state", st, sizeof st) > 0) {
                void *b = fx->create_instance(".", nullptr);
                fx->set_param(b, "state", st);
                static char st2[8192];
                fx->get_param(b, "state", st2, sizeof st2);
                if (strcmp(st, st2) != 0) {
                    stateFails++;
                    printf("  state round trip DIFFERS at block %ld\n", blocks);
                    printf("    a=%.200s\n    b=%.200s\n", st, st2);
                }
                fx->destroy_instance(b);
            }
        }
    }

    printf("\n=== soak: %ld blocks (%.1f s of audio), %ld param writes ===\n",
           blocks, blocks * (double)MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE, writes);
    printf("  mean   %.3f ms/block  (%.1f%% of budget)\n",
           totalMs / (double)blocks, 100.0 * (totalMs / (double)blocks) / budgetMs);
    printf("  WORST  %.3f ms/block  (%.1f%% of budget) at block %d, after %s\n",
           worstMs, 100.0 * worstMs / budgetMs, worstAtBlock, worstKey);
    printf("  blocks over budget: %ld\n", over);
    printf("  railed during SILENCE: %ld blocks, longest run %ld (%.2f s)\n",
           bad, worstRailRun, worstRailRun * (double)MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE);
    printf("  state round-trip failures: %ld\n", stateFails);
    /* Recovery: whatever the fuzzer left it in, zero feedback and a silent
     * input must decay to silence. A stuck tape shows up here and nowhere
     * else — the loudness tests all pass with a NaN circulating. */
    fx->set_param(inst, "intensity", "0");
    fx->set_param(inst, "input_send", "Off");
    double peak = 0;
    for (int b = 0; b < 2000; b++) {
        memset(io, 0, sizeof io);
        fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
        if (b > 1500) for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            const double a = fabs((double)io[i]);
            if (a > peak) peak = a;
        }
    }
    const bool recovered = peak < 8.0;
    printf("  recovery after fuzz: tail peak %.1f / 32768 %s\n",
           peak, recovered ? "(decayed)" : "(STUCK)");
    printf("  %s\n", (over == 0 && stateFails == 0 && worstRailRun < 40 && recovered)
                      ? "PASS" : "INVESTIGATE");
    fx->destroy_instance(inst);
    return (over == 0 && stateFails == 0 && worstRailRun < 40 && recovered) ? 0 : 1;
}
