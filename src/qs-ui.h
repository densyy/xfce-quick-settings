#ifndef QS_UI_H
#define QS_UI_H

#include "qs-core.h"

/* ---- visual tokens (single source of truth) ----
 * Radius 10 everywhere; spacing 12 page/section, 8 inner, 4 tight;
 * icons 24 main actions, 20 list rows, 16 inline meta;
 * type: section small bold dim, title larger bold, sub/status small dim. */
#define QS_MAIN_ICON_PX 24
#define QS_ROW_ICON_PX 20
#define QS_META_ICON_PX 16
#define QS_RADIUS_PX 10
#define QS_PAGE_PAD 12
#define QS_SECTION_GAP 12
#define QS_INNER_GAP 8
#define QS_TIGHT_GAP 4
/* Standard inner padding between a container edge and its content
 * (power buttons, cards, rows, tiles). Reuse everywhere. */
#define QS_CONTENT_PAD 10

/* Adds a style class to a widget. */
void ui_add_class (GtkWidget   *widget,
                   const gchar *cls);
/* Loads the shared stylesheet once. */
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
/* Navigation tile: icon + bold title + dim subtitle (sub_out may be NULL
 * to skip the subtitle, or filled when the caller owns the label). */
GtkWidget *make_nav_tile (GtkWidget    *box,
                          const gchar  *icon_name,
                          const gchar  *title,
                          GtkWidget   **sub_out);
/* Compact vertical tile (icon above label) for the action grid. */
GtkWidget *make_mini_tile (GtkWidget    *box,
                           const gchar  *icon_name,
                           const gchar  *title,
                           GtkWidget   **sub_out);
/* Stat card: single qs-card box holding key/value rows (no bars).
 * Returns the card; pack rows into it with make_stat_row. */
GtkWidget *make_stat_card (GtkWidget *grid,
                           gint       row);
void make_stat_row (GtkWidget    *card,
                    const gchar  *name,
                    GtkWidget   **value_out);
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
