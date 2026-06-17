/* translate.h — CallTranslator: bidirectional IPSC <-> HBP translation. */
#ifndef TRANSLATE_H
#define TRANSLATE_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

typedef struct translator translator;
struct ipsc;
struct hbp;

translator *translator_new(const Config *cfg, ev_loop *loop);
void translator_set_protocols(translator *tr, struct ipsc *ip, struct hbp *hb);
void translator_free(translator *tr);

/* IPSC-side callbacks */
void translator_peer_joined(translator *tr);
void translator_peer_lost(translator *tr);
void translator_ipsc_voice_received(translator *tr, const uint8_t *data, int len,
                                    int ts, int burst_type);
void translator_check_call_timeouts(translator *tr);

/* HBP-side callbacks */
void translator_hbp_connected(translator *tr);
void translator_hbp_disconnected(translator *tr);
void translator_hbp_voice_received(translator *tr, const uint8_t *dmrd, int len);

#endif
