#ifndef QS_DISPLAY_H
#define QS_DISPLAY_H

#include "qs-core.h"

/* Builds the brightness slider + Night Light tile on the shared grid
 * (rows 2-3), plus the Night Light sub-page (4 presets). */
void display_build_section (GtkWidget *grid, GtkWidget *stack, QuickSettings *qs);
/* Refresh slider position from the current system brightness. */
void display_refresh_brightness (QuickSettings *qs);

#endif /* QS_DISPLAY_H */
