#ifndef QS_POPUP_H
#define QS_POPUP_H

#include "qs-core.h"

/* Popup placement + auto-hide helpers (extracted from qs-plugin.c).
 * qs-plugin.c owns the signal connections; this unit owns the logic. */
void qs_popup_position (QuickSettings *qs);
/* Variante com tamanho explícito (por requisição, sem map): usada para
 * mover a janela AINDA UNMAPPED antes do show_all. qs_popup_position()
 * mede via gtk_window_get_size (só válida pós-map) e delega para cá. */
void qs_popup_position_at_size (QuickSettings *qs,
                                gint           popup_width,
                                gint           popup_height);
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
