/* hbp.h — HBP outbound client (peer/repeater side) with auto-reconnect. */
#ifndef HBP_H
#define HBP_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

typedef struct hbp hbp;
struct translator;

hbp *hbp_new(const Config *cfg, struct translator *tr, ev_loop *loop);
void hbp_start(hbp *hb);       /* PERSISTENT mode connects immediately */
void hbp_stop(hbp *hb);        /* clean shutdown: RPTCL + cancel */
void hbp_free(hbp *hb);

void hbp_activate(hbp *hb);    /* TRACKING: IPSC peer registered */
void hbp_deactivate(hbp *hb);  /* TRACKING: IPSC peer lost */

void hbp_send_dmrd(hbp *hb, const uint8_t *data, int len);
int  hbp_is_connected(hbp *hb);

#endif
