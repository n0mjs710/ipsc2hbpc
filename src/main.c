/* main.c — ipsc2hbpc entry point.  Wires IPSC, HBP, and the translator
 * together and runs the event loop (port of ipsc2hbp.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "eventloop.h"
#include "ipsc.h"
#include "hbp.h"
#include "translate.h"

static ev_loop *g_loop = NULL;
static ipsc    *g_ipsc = NULL;
static hbp     *g_hbp  = NULL;

static void on_signal(int signum)
{
    LOGI("ipsc2hbp", "Signal %d received — shutting down", signum);
    if (g_ipsc) ipsc_stop(g_ipsc);
    if (g_hbp)  hbp_stop(g_hbp);
    if (g_loop) ev_stop(g_loop);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c config.toml] [--log-level LEVEL] [--wire]\n"
        "  -c, --config PATH    Path to TOML config (default: ipsc2hbp.toml next to binary)\n"
        "  --log-level LEVEL    Override config log level (DEBUG|INFO|WARNING|ERROR)\n"
        "  --wire               Log raw IPSC/HBP hex only; silence everything else\n",
        prog);
}

int main(int argc, char **argv)
{
    /* Default config: the system location.  For development runs from the repo,
     * pass -c ipsc2hbp.toml explicitly. */
    const char *cfg_path = "/etc/ipsc2hbp/ipsc2hbp.toml";
    const char *log_level_override = NULL;
    int wire = 0;

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
            log_level_override = argv[++i];
        } else if (!strcmp(argv[i], "--wire")) {
            wire = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            usage(argv[0]); return 2;
        }
    }

    Config cfg;
    char err[4096];
    if (config_load(cfg_path, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "Configuration error: %s\n", err);
        return 1;
    }

    int level = cfg.log_level;
    if (log_level_override) {
        int l = log_level_from_str(log_level_override);
        if (l >= 0) level = l;
    }
    log_init(level, wire);

    if (!strcmp(cfg.ipsc_mode, "MASTER")) {
        LOGI("ipsc2hbp", "ipsc2hbp starting — ipsc=MASTER  id=%u  listen=%s:%d  "
                         "hbp_id=%u  upstream=%s:%d  hbp_mode=%s",
             cfg.ipsc_master_id, cfg.ipsc_bind_ip, cfg.ipsc_bind_port,
             cfg.hbp_repeater_id, cfg.hbp_master_ip, cfg.hbp_master_port, cfg.hbp_mode);
    } else {
        LOGI("ipsc2hbp", "ipsc2hbp starting — ipsc=PEER  id=%u  master=%s:%d  local=%s:%d  "
                         "hbp_id=%u  upstream=%s:%d  hbp_mode=%s",
             cfg.ipsc_master_id, cfg.ipsc_master_ip, cfg.ipsc_master_port,
             cfg.ipsc_bind_ip, cfg.ipsc_bind_port,
             cfg.hbp_repeater_id, cfg.hbp_master_ip, cfg.hbp_master_port, cfg.hbp_mode);
    }

    g_loop = ev_new();
    translator *tr = translator_new(&cfg, g_loop);
    g_ipsc = ipsc_new(&cfg, tr, g_loop);
    g_hbp  = hbp_new(&cfg, tr, g_loop);
    translator_set_protocols(tr, g_ipsc, g_hbp);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    if (ipsc_start(g_ipsc) != 0) {
        fprintf(stderr, "Failed to bind IPSC socket: %s:%d\n", cfg.ipsc_bind_ip, cfg.ipsc_bind_port);
        return 1;
    }
    if (!strcmp(cfg.ipsc_mode, "MASTER"))
        LOGI("ipsc2hbp", "IPSC master endpoint up — %s:%d", cfg.ipsc_bind_ip, cfg.ipsc_bind_port);
    else
        LOGI("ipsc2hbp", "IPSC peer socket bound — %s:%d", cfg.ipsc_bind_ip, cfg.ipsc_bind_port);

    hbp_start(g_hbp);

    ev_run(g_loop);

    hbp_free(g_hbp);
    ipsc_free(g_ipsc);
    translator_free(tr);
    ev_free(g_loop);
    LOGI("ipsc2hbp", "ipsc2hbp stopped");
    return 0;
}
