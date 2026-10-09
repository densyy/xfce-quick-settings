#ifndef QS_POWER_H
#define QS_POWER_H

#include "qs-core.h"

/* Builds the Battery group (hidden when no battery is present). */
void power_build_section (GtkWidget *box, QuickSettings *qs);
/* Refresh icon + label; hides the whole section when no battery exists. */
void power_refresh (QuickSettings *qs);

#endif /* QS_POWER_H */
