#ifndef QS_SYSMON_H
#define QS_SYSMON_H

#include "qs-core.h"

/* Builds the System tile + sub-page (RAM/CPU/disks, 10s refresh). */
void sysmon_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);
/* Stops the refresh timer (safe to call when already stopped). */
void sysmon_stop (QuickSettings *qs);

#endif /* QS_SYSMON_H */
