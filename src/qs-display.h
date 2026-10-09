#ifndef QS_DISPLAY_H
#define QS_DISPLAY_H

#include "qs-core.h"

/* Builds the Display group (brightness slider + night light presets). */
void display_build_section (GtkWidget *box, QuickSettings *qs);
/* Refresh slider position from the current system brightness. */
void display_refresh_brightness (QuickSettings *qs);

#endif /* QS_DISPLAY_H */
