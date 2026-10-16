#ifndef QS_DISPLAY_H
#define QS_DISPLAY_H

#include "qs-core.h"

/* Piso anti-tela-preta (era macro local do brightness; global para a
 * fachada impor o mesmo piso no range do slider). */
#define BRIGHTNESS_MIN 5
#define BRIGHTNESS_MIN_XRANDR 30

/* Builds the brightness slider + Night Light/Display tiles on the
 * shared grid, plus the Night Light sub-page (4 presets) and the
 * Display sub-page (monitor/resolution/scale selectors). */
void display_build_section (GtkWidget *grid, GtkWidget *stack, QuickSettings *qs);
/* Refresh slider position from the current system brightness. */
void display_refresh_brightness (QuickSettings *qs);
/* Refresh the Display tile (async xrandr probe, or "unavailable"). */
void display_refresh_tile (QuickSettings *qs);

/* --- brightness (qs-brightness.c) --- */
void on_brightness_changed (GtkRange *range, gpointer user_data);
void bright_stop (void);

/* --- night light (qs-nightlight.c) --- */
gboolean night_available (void);
void night_update_sub (QuickSettings *qs);
GtkWidget *make_night_button (GtkWidget   *box,
                              const gchar *icon_name,
                              const gchar *label,
                              const gchar *temp,
                              gpointer     user_data);

/* --- monitors (qs-monitor.c) --- */
GtkWidget *make_combo_row (GtkWidget   *grid,
                           gint         row,
                           const gchar *icon_name,
                           const gchar *tooltip);
/* Nome de output xrandr ([A-Za-z0-9_.-]). Symlink do validador de monitores. */
gboolean valid_monitor_shared (const gchar *s);
void display_load_async (QuickSettings *qs);
void on_mon_changed (GtkComboBox *combo, gpointer user_data);
void on_mode_changed (GtkComboBox *combo, gpointer user_data);
void on_scale_changed (GtkComboBox *combo, gpointer user_data);
void on_mon_power_changed (GObject *obj, GParamSpec *pspec,
                           gpointer user_data);

#endif /* QS_DISPLAY_H */
