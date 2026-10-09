#ifndef QS_SYSMON_H
#define QS_SYSMON_H

#include "qs-core.h"

/* One disk entry: display name + preformatted "used / total · pct" text. */
typedef struct
{
  gchar   *name;
  gchar   *text; /* preformatted "used / total · pct" */
  gdouble  fraction; /* used / total, 0-1 */
} SysmonDisk;

/* Builds the System tile + an empty sub-page (layout removed; the fresh
 * component goes here). */
void sysmon_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);
/* Stops the refresh timer (safe to call when already stopped). */
void sysmon_stop (QuickSettings *qs);
/* Data layer (kept, no widgets): memory in kB, CPU 0-100, disks as a
 * SysmonDisk list (free with sysmon_free_disks). */
gboolean sysmon_read_memory (guint64 *total_kb,
                             guint64 *avail_kb);
gdouble sysmon_read_cpu (QuickSettings *qs);
GPtrArray *sysmon_read_disks (void);
void sysmon_free_disks (GPtrArray *disks);
gchar *sysmon_bytes_text (guint64 bytes,
                          guint64 total);

#endif /* QS_SYSMON_H */
