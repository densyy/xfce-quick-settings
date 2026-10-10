#ifndef QS_POPUP_H
#define QS_POPUP_H

#include "qs-core.h"

/* Popup placement + auto-hide helpers (extracted from qs-plugin.c).
 * qs-plugin.c owns the signal connections; this unit owns the logic. */
void qs_popup_position (QuickSettings *qs);
/* TRUE while the pointer is over the panel button (i.e. a focus-out was
 * caused by pressing the button itself, not by clicking elsewhere). */
gboolean qs_popup_pointer_on_button (QuickSettings *qs);
gboolean qs_popup_on_focus_out (GtkWidget     *widget,
                                GdkEventFocus *event,
                                gpointer       user_data);
gboolean qs_popup_on_key_press (GtkWidget   *widget,
                                GdkEventKey *event,
                                gpointer     user_data);

#endif /* QS_POPUP_H */
