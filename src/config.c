/* config.c — load and validate the TOML config (port of config.py). */
#include "config.h"
#include "toml.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

/* error accumulator */
typedef struct { char buf[4096]; int n; } errbag;
static void adderr(errbag *e, const char *fmt, ...) {
    char line[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->n += snprintf(e->buf + e->n, sizeof e->buf - (size_t)e->n, "  %s\n", line);
}

static void str_to_upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

/* trim leading/trailing whitespace in place (returns s) */
static char *trimcpy(char *dst, size_t cap, const char *src) {
    while (*src && isspace((unsigned char)*src)) src++;
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len-1])) len--;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    return dst;
}

/* get_str: returns 1 if present (out filled, stripped), 0 if absent/invalid. */
static int get_str(const toml *t, errbag *e, const char *sec, const char *key,
                   int required, const char *deflt, char *out, size_t cap) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) {
        if (required) adderr(e, "[%s] %s: required", sec, key);
        if (deflt) { snprintf(out, cap, "%s", deflt); } else out[0] = 0;
        return 0;
    }
    if (v->type != TOML_STRING) {
        adderr(e, "[%s] %s: must be a string", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    trimcpy(out, cap, v->s);
    return 1;
}

/* get_str with choices (uppercased). */
static int get_choice(const toml *t, errbag *e, const char *sec, const char *key,
                      int required, const char *deflt,
                      const char *const *choices, int nch, char *out, size_t cap) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) {
        if (required) adderr(e, "[%s] %s: required", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    if (v->type != TOML_STRING) { adderr(e, "[%s] %s: must be a string", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0]=0; return 0; }
    char tmp[64]; trimcpy(tmp, sizeof tmp, v->s); str_to_upper(tmp);
    for (int i = 0; i < nch; i++)
        if (!strcmp(tmp, choices[i])) { snprintf(out, cap, "%s", choices[i]); return 1; }
    adderr(e, "[%s] %s: invalid value '%s'", sec, key, v->s);
    if (deflt) snprintf(out, cap, "%s", deflt); else out[0]=0;
    return 0;
}

static long long get_int(const toml *t, errbag *e, const char *sec, const char *key,
                         int required, long long deflt, int has_min, long long mn,
                         int has_max, long long mx) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) { if (required) adderr(e, "[%s] %s: required", sec, key); return deflt; }
    if (v->type != TOML_INT) { adderr(e, "[%s] %s: must be an integer", sec, key); return deflt; }
    if (has_min && v->i < mn) adderr(e, "[%s] %s: must be >= %lld, got %lld", sec, key, mn, v->i);
    if (has_max && v->i > mx) adderr(e, "[%s] %s: must be <= %lld, got %lld", sec, key, mx, v->i);
    return v->i;
}

static int get_bool(const toml *t, errbag *e, const char *sec, const char *key,
                    int required, int deflt) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) { if (required) adderr(e, "[%s] %s: required", sec, key); return deflt; }
    if (v->type != TOML_BOOL) { adderr(e, "[%s] %s: must be true or false", sec, key); return deflt; }
    return v->b;
}

/* capability bool with default (no error if missing) */
static int cap_bool(const toml *t, errbag *e, const char *key, int deflt) {
    const toml_value *v = toml_get(t, "ipsc.capabilities", key);
    if (!v) return deflt;
    if (v->type != TOML_BOOL) { adderr(e, "[ipsc.capabilities] %s: must be true or false", key); return deflt; }
    return v->b;
}

int config_load(const char *path, Config *cfg, char *err, size_t errlen)
{
    char perr[256];
    toml *t = toml_parse_file(path, perr, sizeof perr);
    if (!t) { snprintf(err, errlen, "%s", perr); return -1; }

    errbag e; e.buf[0] = 0; e.n = 0;
    memset(cfg, 0, sizeof *cfg);

    /* [global] */
    { static const char *LV[] = {"DEBUG","INFO","WARNING","ERROR"};
      char lvl[16];
      get_choice(t, &e, "global", "log_level", 1, "INFO", LV, 4, lvl, sizeof lvl);
      cfg->log_level = log_level_from_str(lvl);
      if (cfg->log_level < 0) cfg->log_level = LOG_INFO;
    }

    /* [ipsc] */
    get_str(t, &e, "ipsc", "bind_ip", 1, "", cfg->ipsc_bind_ip, sizeof cfg->ipsc_bind_ip);
    cfg->ipsc_bind_port = (int)get_int(t, &e, "ipsc", "bind_port", 1, 0, 1, 1, 1, 65535);
    cfg->ipsc_master_id = (uint32_t)get_int(t, &e, "ipsc", "ipsc_master_id", 1, 0, 1, 1, 0, 0);

    /* allowed_peer_ids: optional array of ints */
    { const toml_value *v = toml_get(t, "ipsc", "allowed_peer_ids");
      if (v) {
        if (v->type != TOML_ARRAY) adderr(&e, "[ipsc] allowed_peer_ids: must be an array of integers");
        else for (int i = 0; i < v->arrlen; i++) {
            if (v->arr[i].type != TOML_INT) { adderr(&e, "[ipsc] allowed_peer_ids: all entries must be integers"); continue; }
            if (cfg->n_allowed_peer_ids < CFG_MAX_PEERS_LIST)
                cfg->allowed_peer_ids[cfg->n_allowed_peer_ids++] = (uint32_t)v->arr[i].i;
        }
      }
    }
    /* allowed_peer_ips: optional array of IPv4 strings */
    { const toml_value *v = toml_get(t, "ipsc", "allowed_peer_ips");
      if (v) {
        if (v->type != TOML_ARRAY) adderr(&e, "[ipsc] allowed_peer_ips: must be an array of strings");
        else for (int i = 0; i < v->arrlen; i++) {
            if (v->arr[i].type != TOML_STRING) { adderr(&e, "[ipsc] allowed_peer_ips: all entries must be strings"); continue; }
            char ip[16]; trimcpy(ip, sizeof ip, v->arr[i].s);
            struct in_addr a;
            if (inet_pton(AF_INET, ip, &a) != 1) { adderr(&e, "[ipsc] allowed_peer_ips: not a valid IPv4 address: %s", v->arr[i].s); continue; }
            if (cfg->n_allowed_peer_ips < CFG_MAX_PEERS_LIST)
                snprintf(cfg->allowed_peer_ips[cfg->n_allowed_peer_ips++], 16, "%s", ip);
        }
      }
    }

    cfg->auth_enabled       = get_bool(t, &e, "ipsc", "auth_enabled", 1, 0);
    cfg->keepalive_watchdog = (int)get_int(t, &e, "ipsc", "keepalive_watchdog", 1, 0, 1, 5, 0, 0);
    cfg->ipsc_ts_prefer_call_info = get_bool(t, &e, "ipsc", "ts_prefer_call_info", 0, 0);

    { static const char *M[] = {"MASTER","PEER"};
      get_choice(t, &e, "ipsc", "mode", 0, "MASTER", M, 2, cfg->ipsc_mode, sizeof cfg->ipsc_mode); }

    if (!strcmp(cfg->ipsc_mode, "PEER")) {
        get_str(t, &e, "ipsc", "master_ip", 1, "", cfg->ipsc_master_ip, sizeof cfg->ipsc_master_ip);
        cfg->ipsc_master_port = (int)get_int(t, &e, "ipsc", "master_port", 1, 0, 1, 1, 1, 65535);
    } else {
        cfg->ipsc_master_ip[0] = 0;
        cfg->ipsc_master_port = 0;
    }
    cfg->keepalive_interval   = (int)get_int(t, &e, "ipsc", "keepalive_interval", 0, 5, 1, 1, 1, 300);
    cfg->keepalive_missed_max = (int)get_int(t, &e, "ipsc", "keepalive_missed_max", 0, 3, 1, 1, 1, 20);

    /* auth_key */
    memset(cfg->auth_key, 0, 20);
    if (cfg->auth_enabled) {
        const toml_value *v = toml_get(t, "ipsc", "auth_key");
        if (!v || v->type != TOML_STRING) {
            adderr(&e, "[ipsc] auth_key: required");
        } else {
            char raw[64]; trimcpy(raw, sizeof raw, v->s);
            if (strlen(raw) > 40) {
                adderr(&e, "[ipsc] auth_key: must be at most 40 hex characters");
            } else {
                char padded[41];
                int pad = 40 - (int)strlen(raw);
                memset(padded, '0', (size_t)pad);
                strcpy(padded + pad, raw);
                int ok = 1;
                for (int i = 0; i < 40; i++) if (!isxdigit((unsigned char)padded[i])) ok = 0;
                if (!ok) adderr(&e, "[ipsc] auth_key: not valid hex");
                else for (int i = 0; i < 20; i++) {
                    char byte[3] = { padded[i*2], padded[i*2+1], 0 };
                    cfg->auth_key[i] = (uint8_t)strtol(byte, NULL, 16);
                }
            }
        }
    }

    /* [ipsc.capabilities] */
    int use_safe = 1;
    { const toml_value *v = toml_get(t, "ipsc.capabilities", "use_safe_defaults");
      if (v) { if (v->type != TOML_BOOL) adderr(&e, "[ipsc.capabilities] use_safe_defaults: must be true or false");
               else use_safe = v->b; } }

    int is_master = !strcmp(cfg->ipsc_mode, "MASTER");
    if (use_safe) {
        cfg->ipsc_mode_byte = 0x6A;
        int b4 = 0x04;
        if (is_master) b4 |= 0x01;
        if (cfg->auth_enabled) b4 |= 0x10;
        cfg->ipsc_flags_bytes[0] = 0; cfg->ipsc_flags_bytes[1] = 0;
        cfg->ipsc_flags_bytes[2] = 0; cfg->ipsc_flags_bytes[3] = (uint8_t)b4;
        cfg->ipsc_version[0]=0x04; cfg->ipsc_version[1]=0x02;
        cfg->ipsc_version[2]=0x04; cfg->ipsc_version[3]=0x01;
    } else {
        int op = 0b01;
        char rm[16] = "DIGITAL";
        const toml_value *v = toml_get(t, "ipsc.capabilities", "radio_mode");
        if (v) { if (v->type != TOML_STRING) adderr(&e, "[ipsc.capabilities] radio_mode: must be a string");
                 else { trimcpy(rm, sizeof rm, v->s); str_to_upper(rm); } }
        int mode_bits;
        if (!strcmp(rm,"DIGITAL")) mode_bits=0b10; else if (!strcmp(rm,"ANALOG")) mode_bits=0b01;
        else if (!strcmp(rm,"NO_RADIO")) mode_bits=0b00; else if (!strcmp(rm,"MIXED")) mode_bits=0b11;
        else { adderr(&e,"[ipsc.capabilities] radio_mode: must be DIGITAL, ANALOG, NO_RADIO, or MIXED"); mode_bits=0b10; }
        int ts1 = cap_bool(t,&e,"ts1_linked",1), ts2 = cap_bool(t,&e,"ts2_linked",1);
        int ts1b = ts1?0b10:0b01, ts2b = ts2?0b10:0b01;
        cfg->ipsc_mode_byte = (uint8_t)((op<<6)|(mode_bits<<4)|(ts1b<<2)|ts2b);

        int b0=0;
        if (cap_bool(t,&e,"slot2_wireline",0)) b0|=0x10;
        if (cap_bool(t,&e,"slot1_wireline",0)) b0|=0x08;
        if (cap_bool(t,&e,"wireline_svc",0))   b0|=0x04;
        int b1=0;
        if (cap_bool(t,&e,"mnis",0))         b1|=0x80;
        if (cap_bool(t,&e,"ip_site_freq",0)) b1|=0x40;
        if (cap_bool(t,&e,"slot2_phone",0))  b1|=0x10;
        if (cap_bool(t,&e,"slot1_phone",0))  b1|=0x08;
        if (cap_bool(t,&e,"virtual_peer",0)) b1|=0x04;
        if (cap_bool(t,&e,"cps_avail",0))    b1|=0x02;
        int b2=0;
        if (cap_bool(t,&e,"csbk",0))    b2|=0x80;
        if (cap_bool(t,&e,"rpt_mon",0)) b2|=0x40;
        if (cap_bool(t,&e,"con_app",0)) b2|=0x20;
        int b3 = is_master ? 0x01 : 0x00;
        if (cap_bool(t,&e,"xnl_con",0))    b3|=0x80;
        if (cap_bool(t,&e,"xnl_master",0)) b3|=0x40;
        if (cap_bool(t,&e,"xnl_slave",0))  b3|=0x20;
        if (cfg->auth_enabled)             b3|=0x10;
        if (cap_bool(t,&e,"data",0))       b3|=0x08;
        if (cap_bool(t,&e,"voice",1))      b3|=0x04;
        cfg->ipsc_flags_bytes[0]=(uint8_t)b0; cfg->ipsc_flags_bytes[1]=(uint8_t)b1;
        cfg->ipsc_flags_bytes[2]=(uint8_t)b2; cfg->ipsc_flags_bytes[3]=(uint8_t)b3;

        char ver[16] = "04020401";
        const toml_value *vv = toml_get(t, "ipsc.capabilities", "ipsc_version");
        if (vv) { if (vv->type != TOML_STRING) adderr(&e, "[ipsc.capabilities] ipsc_version: must be an 8-character hex string");
                  else { char raw[32]; trimcpy(raw,sizeof raw,vv->s);
                         /* remove spaces */
                         char clean[32]; int cn=0; for (char *p=raw;*p;p++) if(*p!=' ') clean[cn++]=*p; clean[cn]=0;
                         if (cn != 8) adderr(&e,"[ipsc.capabilities] ipsc_version: must be exactly 8 hex characters (4 bytes)");
                         else snprintf(ver,sizeof ver,"%s",clean); } }
        int ok=1; for (int i=0;i<8;i++) if(!isxdigit((unsigned char)ver[i])) ok=0;
        if (!ok) { adderr(&e,"[ipsc.capabilities] ipsc_version: not valid hex"); strcpy(ver,"04020401"); }
        for (int i=0;i<4;i++){ char by[3]={ver[i*2],ver[i*2+1],0}; cfg->ipsc_version[i]=(uint8_t)strtol(by,NULL,16); }
    }

    /* [hbp] */
    get_str(t, &e, "hbp", "master_ip", 1, "", cfg->hbp_master_ip, sizeof cfg->hbp_master_ip);
    cfg->hbp_master_port = (int)get_int(t, &e, "hbp", "master_port", 1, 0, 1, 1, 1, 65535);
    { static const char *HM[] = {"TRACKING","PERSISTENT"};
      get_choice(t, &e, "hbp", "hbp_mode", 1, "TRACKING", HM, 2, cfg->hbp_mode, sizeof cfg->hbp_mode); }
    cfg->hbp_repeater_id = (uint32_t)get_int(t, &e, "hbp", "hbp_repeater_id", 1, 0, 1, 1, 0, 0);

    { char pp[256]; get_str(t, &e, "hbp", "passphrase", 1, "", pp, sizeof pp);
      cfg->hbp_passphrase_len = (int)strlen(pp);
      memcpy(cfg->hbp_passphrase, pp, (size_t)cfg->hbp_passphrase_len); }

    get_str(t, &e, "hbp", "options",     0, "",          cfg->options,     sizeof cfg->options);
    get_str(t, &e, "hbp", "callsign",    0, "NOCALL",    cfg->callsign,    sizeof cfg->callsign);
    get_str(t, &e, "hbp", "rx_freq",     0, "000000000", cfg->rx_freq,     sizeof cfg->rx_freq);
    get_str(t, &e, "hbp", "tx_freq",     0, "000000000", cfg->tx_freq,     sizeof cfg->tx_freq);
    get_str(t, &e, "hbp", "tx_power",    0, "00",        cfg->tx_power,    sizeof cfg->tx_power);
    get_str(t, &e, "hbp", "colorcode",   0, "01",        cfg->colorcode,   sizeof cfg->colorcode);
    get_str(t, &e, "hbp", "latitude",    0, "00.0000 ",  cfg->latitude,    sizeof cfg->latitude);
    get_str(t, &e, "hbp", "longitude",   0, "000.0000 ", cfg->longitude,   sizeof cfg->longitude);
    get_str(t, &e, "hbp", "height",      0, "000",       cfg->height,      sizeof cfg->height);
    get_str(t, &e, "hbp", "location",    0, "",          cfg->location,    sizeof cfg->location);
    get_str(t, &e, "hbp", "description", 0, "",          cfg->description, sizeof cfg->description);
    get_str(t, &e, "hbp", "url",         0, "",          cfg->url,         sizeof cfg->url);
    get_str(t, &e, "hbp", "software_id", 0, "ipsc2hbp",  cfg->software_id, sizeof cfg->software_id);
    get_str(t, &e, "hbp", "package_id",  0, "1.0.0",     cfg->package_id,  sizeof cfg->package_id);

    toml_free(t);

    if (e.n > 0) {
        snprintf(err, errlen, "Configuration errors:\n%s", e.buf);
        return -1;
    }
    return 0;
}
