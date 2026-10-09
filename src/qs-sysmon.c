#include "qs-sysmon.h"
#include "qs-ui.h"

#include <sys/statvfs.h>

/* Pseudo-filesystems never shown as disks. */
static const gchar *skip_fs[] = {
  "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "securityfs",
  "cgroup", "cgroup2", "pstore", "debugfs", "tracefs", "configfs",
  "fusectl", "mqueue", "hugetlbfs", "nsfs", "overlay", "squashfs", NULL
};

static gboolean
fs_skipped (const gchar *fstype,
            const gchar *device)
{
  gint i;

  for (i = 0; skip_fs[i] != NULL; i++)
    if (g_strcmp0 (fstype, skip_fs[i]) == 0)
      return TRUE;
  if (g_str_has_prefix (device, "/dev/loop"))
    return TRUE;
  if (g_str_has_prefix (fstype, "fuse.") && g_strcmp0 (fstype, "fuseblk") != 0)
    return TRUE;
  return FALSE;
}

gboolean
sysmon_read_memory (guint64 *total_kb,
                    guint64 *avail_kb)
{
  gchar  *content = NULL;
  gchar **lines;
  gint    i;
  gboolean have_total = FALSE;
  gboolean have_avail = FALSE;
  guint64  free_kb = 0;
  guint64  buffers_kb = 0;
  guint64  cached_kb = 0;

  if (!g_file_get_contents ("/proc/meminfo", &content, NULL, NULL))
    return FALSE;
  lines = g_strsplit (content, "\n", -1);
  g_free (content);
  for (i = 0; lines[i] != NULL; i++)
    {
      if (g_str_has_prefix (lines[i], "MemTotal:"))
        {
          *total_kb = g_ascii_strtoull (lines[i] + strlen ("MemTotal:"), NULL, 10);
          have_total = TRUE;
        }
      else if (g_str_has_prefix (lines[i], "MemAvailable:"))
        {
          *avail_kb = g_ascii_strtoull (lines[i] + strlen ("MemAvailable:"), NULL, 10);
          have_avail = TRUE;
        }
      else if (g_str_has_prefix (lines[i], "MemFree:"))
        free_kb = g_ascii_strtoull (lines[i] + strlen ("MemFree:"), NULL, 10);
      else if (g_str_has_prefix (lines[i], "Buffers:"))
        buffers_kb = g_ascii_strtoull (lines[i] + strlen ("Buffers:"), NULL, 10);
      else if (g_str_has_prefix (lines[i], "Cached:"))
        cached_kb = g_ascii_strtoull (lines[i] + strlen ("Cached:"), NULL, 10);
    }
  g_strfreev (lines);
  if (!have_total)
    return FALSE;
  if (!have_avail)
    *avail_kb = free_kb + buffers_kb + cached_kb;
  return TRUE;
}

/* CPU usage 0-100. First call uses since-boot averages, then deltas. */
gdouble
sysmon_read_cpu (QuickSettings *qs)
{
  gchar   *content = NULL;
  gchar  **lines;
  gchar  **fields;
  guint64  nums[10] = { 0 };
  guint64  total = 0;
  guint64  idle;
  gint     i;
  gint     n = 0;
  gdouble  percent;

  if (!g_file_get_contents ("/proc/stat", &content, NULL, NULL))
    return -1.0;
  lines = g_strsplit (content, "\n", 3);
  g_free (content);
  if (lines[0] == NULL || !g_str_has_prefix (lines[0], "cpu "))
    {
      g_strfreev (lines);
      return -1.0;
    }
  fields = g_strsplit (g_strstrip (lines[0] + strlen ("cpu ")), " ", -1);
  g_strfreev (lines);
  for (i = 0; fields[i] != NULL && n < 10; i++)
    {
      if (fields[i][0] == '\0')
        continue;
      nums[n] = g_ascii_strtoull (fields[i], NULL, 10);
      total += nums[n];
      n++;
    }
  g_strfreev (fields);
  idle = nums[3] + nums[4]; /* idle + iowait */
  if (total == 0)
    return -1.0;
  if (!qs->sysmon_have_prev || total <= qs->sysmon_prev_total)
    percent = 100.0 * (1.0 - (gdouble) idle / (gdouble) total);
  else
    {
      guint64 d_total = total - qs->sysmon_prev_total;
      guint64 d_idle = (idle >= qs->sysmon_prev_idle)
                       ? idle - qs->sysmon_prev_idle : 0;

      percent = (d_total == 0) ? 0.0
                               : 100.0 * (1.0 - (gdouble) d_idle / (gdouble) d_total);
    }
  qs->sysmon_prev_total = total;
  qs->sysmon_prev_idle = idle;
  qs->sysmon_have_prev = TRUE;
  return CLAMP (percent, 0.0, 100.0);
}

gchar *
sysmon_bytes_text (guint64 bytes,
                   guint64 total)
{
  gdouble pct = (total == 0) ? 0.0
                             : 100.0 * (gdouble) bytes / (gdouble) total;

  return g_strdup_printf ("%.1f / %.1f GB · %d%%",
                          bytes / 1073741824.0, total / 1073741824.0,
                          (gint) (pct + 0.5));
}

/* /proc/mounts escapes space/tab/newline/backslash as octal. */
static gchar *
unescape_mount (const gchar *s)
{
  GString *out = g_string_new (NULL);
  const gchar *p = s;

  while (*p != '\0')
    {
      if (p[0] == '\\' && p[1] == '0'
          && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7')
        {
          if (p[2] == '4' && p[3] == '0')
            g_string_append_c (out, ' ');
          else if (p[2] == '1' && p[3] == '1')
            g_string_append_c (out, '\t');
          else if (p[2] == '1' && p[3] == '2')
            g_string_append_c (out, '\n');
          else if (p[2] == '3' && p[3] == '4')
            g_string_append_c (out, '\\');
          else
            g_string_append_len (out, p, 4);
          p += 4;
        }
      else
        g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

static void
disk_elem_free (gpointer data)
{
  SysmonDisk *disk = data;

  g_free (disk->name);
  g_free (disk->text);
  g_free (disk);
}

void
sysmon_free_disks (GPtrArray *disks)
{
  if (disks == NULL)
    return;
  g_ptr_array_free (disks, TRUE);
}

/* One entry per device: the same partition often appears under several
 * mount points (bind mounts, btrfs subvolumes). Caller frees the list
 * with sysmon_free_disks. */
GPtrArray *
sysmon_read_disks (void)
{
  GPtrArray *disks = g_ptr_array_new_with_free_func (disk_elem_free);
  gchar     *content = NULL;
  gchar    **lines;
  gint        i;

  if (!g_file_get_contents ("/proc/mounts", &content, NULL, NULL))
    return disks;
  {
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);

    lines = g_strsplit (content, "\n", -1);
    g_free (content);
    for (i = 0; lines[i] != NULL; i++)
      {
        gchar **parts = g_strsplit (lines[i], " ", 4);
        struct statvfs st;
        guint64 total;
        guint64 used;
        gchar *device;
        gchar *target;
        const gchar *base;
        SysmonDisk *disk;

        if (g_strv_length (parts) < 3)
          {
            g_strfreev (parts);
            continue;
          }
        device = unescape_mount (parts[0]);
        target = unescape_mount (parts[1]);
        if (fs_skipped (parts[2], device)
            || statvfs (target, &st) != 0
            || st.f_blocks == 0
            || st.f_frsize == 0
            || g_hash_table_contains (seen, device))
          {
            g_free (device);
            g_free (target);
            g_strfreev (parts);
            continue;
          }
        g_hash_table_add (seen, g_strdup (device));
        total = (guint64) st.f_blocks * st.f_frsize;
        used = (guint64) (st.f_blocks - st.f_bfree) * st.f_frsize;
        base = device;
        if (g_str_has_prefix (device, "/dev/"))
          base += strlen ("/dev/");
        disk = g_new (SysmonDisk, 1);
        disk->name = g_strdup (base);
        disk->text = sysmon_bytes_text (used, total);
        g_ptr_array_add (disks, disk);
        g_free (device);
        g_free (target);
        g_strfreev (parts);
      }
    g_strfreev (lines);
    g_hash_table_destroy (seen);
  }
  return disks;
}

static void
on_sysmon_tile_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "sysmon");
}

static void
on_sysmon_back_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  show_main_page (qs);
}

void
sysmon_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *sys_tile;
  GtkWidget *page;
  GtkWidget *back;

  /* Mini tile in the shared action grid. */
  sys_tile = make_mini_tile (box, "computer-symbolic", "System", NULL);
  g_signal_connect (sys_tile, "clicked",
                    G_CALLBACK (on_sysmon_tile_clicked), qs);

  /* Sub-page shell only: the fresh component goes here. */
  page = make_sub_page ("System", &back);
  gtk_stack_add_named (GTK_STACK (stack), page, "sysmon");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_sysmon_back_clicked), qs);
}
