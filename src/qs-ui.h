#ifndef QS_UI_H
#define QS_UI_H

#include "qs-audio.h"

/* Loads the tile CSS once. */
void ui_load_css (void);
/* Titled group inside box. Returns the content grid for attaching rows. */
GtkWidget *make_group (GtkWidget   *box,
                       const gchar *title);
void make_slider_row (GtkWidget    *grid,
                      gint          row,
                      gdouble       max,
                      const gchar  *icon_name,
                      const gchar  *tooltip,
                      GtkWidget   **icon_out,
                      GtkWidget   **scale_out);
GtkWidget *make_device_row (GtkWidget   *grid,
                            gint         row,
                            const gchar *icon_name,
                            const gchar *tooltip);
void fill_device_combo (GtkWidget *combo,
                        GList     *devs,
                        const gchar *empty_text);
void set_tile_active (GtkWidget *tile,
                      gboolean   active);
GtkWidget *make_tile (GtkWidget   *box,
                      const gchar *icon_name,
                      const gchar *label);
/* Generic sub-page: header (back + title + switch), scan button,
 * scrollable list, status line. */
GtkWidget *build_device_page (const gchar *title,
                              const gchar *scan_label,
                              GtkWidget  **switch_out,
                              GtkWidget  **scan_out,
                              GtkWidget  **list_out,
                              GtkWidget  **status_out,
                              GtkWidget  **back_out);
void clear_list_box (GtkWidget *list);
GtkWidget *make_placeholder_row (const gchar *text);

#endif /* QS_UI_H */
