#ifndef QS_NOTIFY_H
#define QS_NOTIFY_H

#include "qs-core.h"

/* Builds the Notifications tile + sub-page (system notification history). */
void notify_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);
/* Refreshes the tile subtitle with the current history count. */
void notify_update_tile (QuickSettings *qs);

#endif /* QS_NOTIFY_H */
