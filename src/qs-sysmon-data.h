#ifndef QS_SYSMON_DATA_H
#define QS_SYSMON_DATA_H

#include "qs-sysmon.h"

/* DATA (sem widgets): memory/CPU/disks + helpers puros.
 * SysmonDisk reutilizado de qs-sysmon.h. */

gboolean sysmon_read_memory (guint64 *total_kb,
                             guint64 *avail_kb);
gdouble sysmon_read_cpu (QuickSettings *qs);
GPtrArray *sysmon_read_disks (void);
void sysmon_free_disks (GPtrArray *disks);
gchar *sysmon_bytes_text (guint64 bytes,
                          guint64 total);
gchar *sysmon_mounts_key (void);
gchar *unescape_mount (const gchar *s);
void disk_elem_free (gpointer data);

#endif /* QS_SYSMON_DATA_H */
