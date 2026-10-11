/* qs-monitor-data.h — snapshot xrandr DATA (movido de qs-monitor.c).
 * Movimento puro; UI (combos/switch/callbacks/refresh) permanece em qs-monitor.c.
 * NOTA: plano citava "parse helper:226-343" — intervalo real verificado no
 * arquivo: qs_snap_find 226-242 é DATA; 244-343 (display_update_power_switch,
 * make_combo_row, fill_combo) é VIEW e PERMANECE em qs-monitor.c.
 * NOTA: SCALE_FACTORS (qs-monitor.c:29) é compartilhado por valid_scale
 * (DATA) e fill_scale_combo (VIEW); movido para este header como static
 * para G_N_ELEMENTS funcionar nos 2 TUs sem mudar lógica. */

#ifndef QS_MONITOR_DATA_H
#define QS_MONITOR_DATA_H

#include "qs-core.h"

static const gchar *SCALE_FACTORS[] = { "1", "1.25", "1.5", "1.75", "2" };

typedef struct
{
  gchar    *name;
  GList    *modes;         /* gchar*, na ordem do xrandr */
  gchar    *current;       /* modo com '*', ou NULL */
  gboolean  header_active; /* header tem geometria WxH+X+Y */
  gboolean  active;         /* header_active || current != NULL */
} QsMonInfo;

typedef struct
{
  GList *mons; /* QsMonInfo*, na ordem do dump */
  gint   n_connected;
  gint   n_active;
} QsDisplaySnap;

gboolean     valid_monitor_shared (const gchar *s);
gboolean     valid_mode           (const gchar *s);
gboolean     valid_scale          (const gchar *s);
void         qs_mon_info_free     (gpointer data);
void         qs_display_snap_free (QsDisplaySnap *snap);
gboolean     header_has_geometry  (const gchar *header);
QsDisplaySnap *qs_display_snap_new (const gchar *output);
QsMonInfo   *qs_snap_find         (QsDisplaySnap *snap,
                                  const gchar   *name);

#endif /* QS_MONITOR_DATA_H */
