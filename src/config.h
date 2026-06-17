/* config.h — parsed, validated configuration (mirrors config.py Config). */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_PEERS_LIST 64

typedef struct {
    int      log_level;          /* LOG_* enum */

    /* [ipsc] */
    char     ipsc_bind_ip[64];
    int      ipsc_bind_port;
    uint32_t ipsc_master_id;

    uint32_t allowed_peer_ids[CFG_MAX_PEERS_LIST];
    int      n_allowed_peer_ids;
    char     allowed_peer_ips[CFG_MAX_PEERS_LIST][16];
    int      n_allowed_peer_ips;

    int      auth_enabled;
    uint8_t  auth_key[20];
    int      keepalive_watchdog;

    /* [ipsc.capabilities] — computed wire bytes */
    uint8_t  ipsc_mode_byte;        /* 1 byte */
    uint8_t  ipsc_flags_bytes[4];   /* 4 bytes */
    uint8_t  ipsc_version[4];       /* 4 bytes */

    int      ipsc_ts_prefer_call_info;

    /* connection mode */
    char     ipsc_mode[8];          /* "MASTER" | "PEER" */
    char     ipsc_master_ip[256];
    int      ipsc_master_port;
    int      keepalive_interval;
    int      keepalive_missed_max;

    /* [hbp] */
    char     hbp_master_ip[256];
    int      hbp_master_port;
    uint32_t hbp_repeater_id;
    char     hbp_passphrase[256];
    int      hbp_passphrase_len;
    char     hbp_mode[16];          /* "TRACKING" | "PERSISTENT" */

    /* RPTC announcement fields */
    char     options[512];
    char     callsign[64];
    char     rx_freq[32];
    char     tx_freq[32];
    char     tx_power[16];
    char     colorcode[16];
    char     latitude[32];
    char     longitude[32];
    char     height[16];
    char     location[64];
    char     description[64];
    char     url[256];
    char     software_id[64];
    char     package_id[64];
} Config;

/* Load and validate a TOML config file.  Returns 0 on success; on failure
 * returns -1 and fills err with a human-readable message. */
int config_load(const char *path, Config *cfg, char *err, size_t errlen);

#endif
