#ifndef QS_CORE_H
#define QS_CORE_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

/* Plugin version (single source of truth). */
#define QS_VERSION "1.0.0"

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
  GtkWidget       *wifi_sub;
  GtkWidget       *bt_tile;
  GtkWidget       *bt_sub;
  GtkWidget       *audio_sub;
  GtkWidget       *notify_sub;
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
  GtkWidget       *night_tile;
  GtkWidget       *night_sub;
  GtkWidget       *display_tile;
  GtkWidget       *display_sub;
  GtkWidget       *mon_combo;
  GtkWidget       *mon_switch; /* monitor on/off toggle (Display page) */
  GtkWidget       *mode_combo;
  GtkWidget       *scale_combo;
  GtkWidget       *display_status;
  gchar           *display_last; /* cached "xrandr --query" output */
  gboolean         display_loading; /* async query in flight */
  GtkWidget       *bat_box;
  GtkWidget       *bat_icon;
  GtkWidget       *bat_label;
  GtkWidget       *bat_status;
  guint            power_timer; /* 30s refresh, only while popup is open */
  GtkWidget       *notify_list;
  GtkWidget       *notify_clear;
  GtkWidget       *sysmon_ram_value;
  GtkWidget       *sysmon_ram_bar;
  GtkWidget       *sysmon_ram_pct;
  GtkWidget       *sysmon_cpu_value;
  GtkWidget       *sysmon_cpu_bar;
  GtkWidget       *sysmon_cpu_pct;
  GtkWidget       *sysmon_disk_box;
  gchar           *sysmon_disk_key; /* rebuild disk blocks only on change */
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
  guint            cache_seq; /* monotonic anti-stale counter (T3/T4/T5) */
  guint            fade_tick; /* tick callback id for 150ms open fade (0 = none) */
  gint             refcount; /* async ops hold a ref; free at zero */
  gboolean         dead; /* TRUE after free-data: don't touch widgets */
} QuickSettings;

/* Reference counting for async callbacks (defined in qs-core.c). */
void qs_ref (QuickSettings *qs);
void qs_unref (QuickSettings *qs);

#endif /* QS_CORE_H */
