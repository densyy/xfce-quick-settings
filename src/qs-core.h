#ifndef QS_CORE_H
#define QS_CORE_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

/* Plugin state shared by all modules. Widgets are created by each
 * module's build function and refreshed by its refresh function. */
typedef struct
{
  GtkWidget       *button;
  GtkWidget       *button_image;
  GtkWidget       *popup;
  GtkWidget       *vol_icon;
  GtkWidget       *vol_scale;
  GtkWidget       *bright_scale;
  GtkWidget       *sink_combo;
  GtkWidget       *source_combo;
  gint             audio_backend; /* AudioBackend, see qs-audio.h */
  GtkWidget       *stack;
  GtkWidget       *wifi_tile;
  GtkWidget       *bt_tile;
  GtkWidget       *wifi_switch;
  GtkWidget       *wifi_list;
  GtkWidget       *wifi_scan;
  GtkWidget       *wifi_status;
  GtkWidget       *bt_switch;
  GtkWidget       *bt_list;
  GtkWidget       *bt_scan;
  GtkWidget       *bt_status;
  GtkWidget       *night_day;
  GtkWidget       *night_aft;
  GtkWidget       *night_night;
  GtkWidget       *night_dawn;
  GtkWidget       *bat_box;
  GtkWidget       *bat_icon;
  GtkWidget       *bat_label;
  GtkWidget       *notify_list;
  GtkWidget       *sysmon_ram_bar;
  GtkWidget       *sysmon_cpu_bar;
  GtkWidget       *sysmon_disk_box;
  guint            sysmon_timer;
  guint64          sysmon_prev_total;
  guint64          sysmon_prev_idle;
  gboolean         sysmon_have_prev;
  gboolean         wifi_on;
  gboolean         bt_on; /* live-probed on popup open (async, see qs-net) */
  gchar           *bt_controller; /* owned MAC or NULL */
  gboolean         scanning_wifi;
  gboolean         scanning_bt;
  gboolean         bt_tile_probe; /* tile-only BT probe in flight */
  gboolean         updating; /* TRUE while loading current values */
  gint             refcount; /* async ops hold a ref; free at zero */
  gboolean         dead; /* TRUE after free-data: don't touch widgets */
} QuickSettings;

/* Reference counting for async callbacks (defined in qs-plugin.c). */
void qs_ref (QuickSettings *qs);
void qs_unref (QuickSettings *qs);

#endif /* QS_CORE_H */
