/* ipsc.h — IPSC protocol stack (master and peer modes). */
#ifndef IPSC_H
#define IPSC_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

typedef struct ipsc ipsc;
struct translator;

ipsc *ipsc_new(const Config *cfg, struct translator *tr, ev_loop *loop);
int   ipsc_start(ipsc *ip);     /* bind socket + start timers; 0 on success, -1 on bind error */
void  ipsc_stop(ipsc *ip);      /* peer mode: send DE_REG_REQ */
void  ipsc_free(ipsc *ip);

/* Send a pre-built GROUP_VOICE packet (master: to all peers; peer: to master). */
void  ipsc_send_voice(ipsc *ip, const uint8_t *pkt, int len);
int   ipsc_has_peers(ipsc *ip);

#endif
