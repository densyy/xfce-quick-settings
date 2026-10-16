#ifndef QS_POWER_H
#define QS_POWER_H

#include "qs-core.h"

/* Builds the Battery group (hidden when no battery is present). */
void power_build_section (GtkWidget *box, QuickSettings *qs);
/* Refresh icon + label; disables the section when no battery exists. */
void power_refresh (QuickSettings *qs);
/* One refresh + starts the 30s timer (no-op when hidden or running). */
void power_start (QuickSettings *qs);
/* Stops the 30s timer. */
void power_stop (QuickSettings *qs);

/* Pure helpers exposed for tests (no widget access). */
gchar       *power_parse_battery_path (const gchar *output);
gboolean     power_parse_detail (const gchar *output,
                                 gint        *percent,
                                 gchar      **state_out,
                                 gchar      **time_out);
const gchar *battery_icon_for_level (gint percent, gboolean charging);

#endif /* QS_POWER_H */
