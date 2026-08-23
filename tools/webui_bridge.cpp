/* te2_webui_bridge — run the REAL plugin behind the web UI, on a laptop.
 *
 *   te2_webui_bridge <module.dylib|.so>
 *
 * Line protocol on stdin, one response line per command on stdout:
 *
 *   P             -> the chain_params JSON, verbatim from get_param
 *   D             -> {"key":"value", ...} for every param, plus echo_note_name
 *   S <key> <val> -> set_param, then the same dump as D
 *
 * The point is that the macros are REAL PLUGIN PARAMS: turning `time_ms` here
 * runs te2_apply_macro inside the plugin, so the dump that comes back has
 * repeat_rate (or echo_rate_note) already moved. That is exactly what the web
 * UI is being checked for, and a mock bridge could not show it.
 *
 * No audio is rendered — this is the parameter surface only.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dlfcn.h>

#include "../src/host/plugin_api_v1.h"
#include "../src/host/audio_fx_api_v2.h"
#include "../src/dsp/te2_params.h"

static void host_log(const char *) {}
static float host_bpm(void) { return 120.0f; }

static audio_fx_api_v2_t *fx = nullptr;
static void *inst = nullptr;

static std::string get(const char *key)
{
    char buf[4096];
    buf[0] = 0;
    const int n = fx->get_param(inst, key, buf, (int)sizeof buf);
    if (n <= 0) return std::string();
    return std::string(buf);
}

/* JSON string escape, enough for our values (no control chars in practice) */
static std::string esc(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

/* The STATE blob verbatim — which is exactly what a remote UI receives.
 *
 * schwung-manager pushes values by reading "<comp>:state" (fetchAllParams) and
 * NOT by walking chain_params, so a key served only by get_param never reaches
 * the browser. Dumping the individual params here instead was a harness that
 * was kinder than the device: the panel's note readout worked locally and was
 * blank on a Move. Serve what the manager serves. */
static void dump(void)
{
    const std::string st = get("state");
    printf("%s\n", (st.empty() || st[0] != '{') ? "{}" : st.c_str());
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: te2_webui_bridge <module>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) { fprintf(stderr, "no %s in %s\n", AUDIO_FX_INIT_V2_SYMBOL, argv[1]); return 1; }

    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version = 1;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log;
    host.get_bpm = host_bpm;

    fx = init(&host);
    if (!fx) { fprintf(stderr, "init returned null\n"); return 1; }
    inst = fx->create_instance(".", nullptr);
    if (!inst) { fprintf(stderr, "create_instance failed\n"); return 1; }

    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (line[0] == 'P') {
            const std::string p = get("chain_params");
            printf("%s\n", p.empty() ? "[]" : p.c_str());
            fflush(stdout);
        } else if (line[0] == 'D') {
            dump();
        } else if (line[0] == 'S') {
            char *k = line + 1;
            while (*k == ' ') k++;
            char *v = strchr(k, ' ');
            if (v) { *v++ = 0; fx->set_param(inst, k, v); }
            dump();
        } else {
            printf("{}\n");
            fflush(stdout);
        }
    }
    fx->destroy_instance(inst);
    return 0;
}
