#ifndef QS_UI_H
#define QS_UI_H

#include "qs-core.h"

/* Loads the tile CSS once. */
void ui_load_css (void);
/* Titled group inside box. Returns the content grid for attaching rows. */
GtkWidget *make_group (GtkWidget   *box,
                       const gchar *title);
/* Headerless grid with the same spacing (for shared sections). */
GtkWidget *make_plain_grid (GtkWidget *box);
void make_slider_row (GtkWidget    *grid,
                      gint          row,
                      gdouble       max,
                      const gchar  *icon_name,
                      const gchar  *tooltip,
                      GtkWidget   **icon_out,
                      GtkWidget   **scale_out);
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
/* Sub-page with header (back + title); pack custom content into page. */
GtkWidget *make_sub_page (const gchar *title,
                          GtkWidget  **back_out);
void show_main_page (QuickSettings *qs);

#endif /* QS_UI_H */
