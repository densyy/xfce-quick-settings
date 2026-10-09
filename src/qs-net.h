#ifndef QS_NET_H
#define QS_NET_H

#include "qs-core.h"

/* Builds the Connection group (Wi-Fi/Bluetooth tiles + sub-pages). */
void net_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);
/* Live tile state (sensitive/active) for Wi-Fi and Bluetooth. */
void net_update_tiles (QuickSettings *qs);
/* Async live probe of the Bluetooth tile (controller powered state).
 * Safe to call on every popup open; never blocks the main loop. */
void net_refresh_bt_tile (QuickSettings *qs);

#endif /* QS_NET_H */
