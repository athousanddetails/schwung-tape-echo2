/* te2_bench — what does one instance cost, and how many fit?
 *
 *   ./te2_bench <module.so> [maxInstances]
 *
 * Renders N instances back to back inside one block period, the way the chain
 * host does, and reports the per-block cost against BOTH budgets:
 *
 *   2.902 ms  the block period (128 frames at 44.1 kHz) — what the loadtest
 *             prints, and what matters if the DSP had the core to itself.
 *   ~900 us   what is actually left for DSP after the ~2 ms SPI transfer, per
 *             docs/REALTIME_SAFETY.md. This is the real ceiling on a Move and
 *             it is the number to quote.
 *
 * N instances rather than N x one instance: cache pressure is part of the
 * answer, and a delay line per instance is the thing that thrashes it.
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "./tape-echo2.so";
    const int maxN = argc > 2 ? atoi(argv[2]) : 8;

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) { printf("no init symbol in %s\n", path); return 1; }

    host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = 1; host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log; host.get_bpm = host_bpm;
    audio_fx_api_v2_t *fx = init(&host);

    const double blockMs = 1000.0 * MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE;
    const double dspMs   = 0.900;   /* what is left after the SPI transfer */

    printf("%s\n", path);
    printf("  N   total ms/blk   worst      %% of 2.902ms   %% of 900us\n");

    for (int n = 1; n <= maxN; n++) {
        void *inst[16] = {0};
        for (int i = 0; i < n; i++) {
            inst[i] = fx->create_instance(".", nullptr);
            if (!inst[i]) { printf("  create_instance %d failed\n", i); return 1; }
            /* a working setting, not the default: feedback up so the loop is
             * actually doing something, and reverb in circuit */
            fx->set_param(inst[i], "mode", "H2+R");
            fx->set_param(inst[i], "intensity", "0.6");
            fx->set_param(inst[i], "mix", "0.5");
            /* old TapeDelay ignores unknown keys, so the same calls serve both */
            fx->set_param(inst[i], "feedback", "0.6");
        }
        static int16_t io[MOVE_FRAMES_PER_BLOCK * 2];
        /* warm up: fill the delay lines and settle the smoothers */
        for (int b = 0; b < 600; b++) {
            for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
                const double s = 0.3 * sin(2.0 * M_PI * 220.0 * (b * MOVE_FRAMES_PER_BLOCK + i) / MOVE_SAMPLE_RATE);
                io[i*2] = io[i*2+1] = (int16_t)(s * 32000);
            }
            for (int i = 0; i < n; i++) fx->process_block(inst[i], io, MOVE_FRAMES_PER_BLOCK);
        }
        double total = 0, worst = 0;
        const int RUNS = 3000;
        for (int b = 0; b < RUNS; b++) {
            for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
                const double s = 0.3 * sin(2.0 * M_PI * 220.0 * (b * MOVE_FRAMES_PER_BLOCK + i) / MOVE_SAMPLE_RATE);
                io[i*2] = io[i*2+1] = (int16_t)(s * 32000);
            }
            const clock_t t0 = clock();
            for (int i = 0; i < n; i++) fx->process_block(inst[i], io, MOVE_FRAMES_PER_BLOCK);
            const double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
            total += ms;
            if (ms > worst) worst = ms;
        }
        const double mean = total / RUNS;
        printf("  %-3d %8.3f     %8.3f    %8.1f%%      %8.1f%%%s\n",
               n, mean, worst, 100.0 * mean / blockMs, 100.0 * mean / dspMs,
               mean > dspMs ? "   <-- over the DSP budget" : "");
        for (int i = 0; i < n; i++) fx->destroy_instance(inst[i]);
    }
    return 0;
}
