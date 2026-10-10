#ifndef QS_DISPLAY_H
#define QS_DISPLAY_H

#include "qs-core.h"

/* Builds the brightness slider + Night Light/Display tiles on the
 * shared grid, plus the Night Light sub-page (4 presets) and the
 * Display sub-page (monitor/resolution/scale selectors). */
void display_build_section (GtkWidget *grid, GtkWidget *stack, QuickSettings *qs);
/* Refresh slider position from the current system brightness. */
void display_refresh_brightness (QuickSettings *qs);
/* Refresh the Display tile (async xrandr probe, or "unavailable"). */
void display_refresh_tile (QuickSettings *qs);

#endif /* QS_DISPLAY_H */
