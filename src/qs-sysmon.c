#include "qs-sysmon.h"
#include "qs-ui.h"
#include "qs-util.h"

#include <sys/statvfs.h>

#define SYSMON_INTERVAL_S 10

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

static gboolean
read_meminfo (guint64 *total_kb,
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
static gdouble
read_cpu_percent (QuickSettings *qs)
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

static gchar *
gb_text (guint64 bytes,
         guint64 total)
{
  return g_strdup_printf ("%.1f / %.1f GB",
                          bytes / 1073741824.0, total / 1073741824.0);
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
sysmon_refresh (QuickSettings *qs)
{
  guint64  total_kb = 0;
  guint64  avail_kb = 0;
  guint64  used_bytes;
  guint64  total_bytes;
  gdouble  cpu;
  gchar   *text;
  gchar   *content = NULL;
  gchar  **lines;
  gint     i;
  gint     row = 0;
  GtkWidget *disk_grid;

  /* RAM + CPU meters (fixed rows). */
  if (read_meminfo (&total_kb, &avail_kb) && total_kb > 0)
    {
      used_bytes = (total_kb - MIN (avail_kb, total_kb)) * 1024;
      total_bytes = total_kb * 1024;
      text = gb_text (used_bytes, total_bytes);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (qs->sysmon_ram_bar),
                                     (gdouble) used_bytes / (gdouble) total_bytes);
      gtk_label_set_text (GTK_LABEL (qs->sysmon_ram_value), text);
      g_free (text);
    }
  cpu = read_cpu_percent (qs);
  if (cpu >= 0.0)
    {
      text = g_strdup_printf ("%d%%", (gint) (cpu + 0.5));
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (qs->sysmon_cpu_bar),
                                     cpu / 100.0);
      gtk_label_set_text (GTK_LABEL (qs->sysmon_cpu_value), text);
      g_free (text);
    }

  /* Disks: rebuild the rows (partitions may come and go). */
  clear_list_box (qs->sysmon_disk_box);
  disk_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (disk_grid), QS_INNER_GAP);
  gtk_grid_set_column_spacing (GTK_GRID (disk_grid), QS_PAGE_PAD);
  gtk_box_pack_start (GTK_BOX (qs->sysmon_disk_box), disk_grid, FALSE, FALSE, 0);
  if (g_file_get_contents ("/proc/mounts", &content, NULL, NULL))
    {
      /* One row per device: the same partition often appears under
       * several mount points (bind mounts, btrfs subvolumes). */
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
          gchar *name;

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
          name = g_strdup (base);
          g_free (device);
          g_free (target);
          {
            GtkWidget *value = NULL;
            GtkWidget *bar = NULL;

            make_meter (disk_grid, row++, name, &value, &bar);
            text = gb_text (used, total);
            gtk_label_set_text (GTK_LABEL (value), text);
            g_free (text);
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar),
                                           total == 0 ? 0.0
                                           : (gdouble) used / (gdouble) total);
          }
          g_free (name);
          g_strfreev (parts);
        }
      g_strfreev (lines);
      g_hash_table_destroy (seen);
    }
  if (row == 0)
    {
      GtkWidget *empty = gtk_label_new ("No disks found");

      ui_add_class (empty, "qs-status");
      gtk_widget_set_halign (empty, GTK_ALIGN_START);
      gtk_grid_attach (GTK_GRID (disk_grid), empty, 0, 0, 2, 1);
    }
  gtk_widget_show_all (qs->sysmon_disk_box);
}

static gboolean
sysmon_tick (gpointer data)
{
  QuickSettings *qs = data;

  if (qs->dead)
    {
      qs->sysmon_timer = 0;
      return G_SOURCE_REMOVE;
    }
  sysmon_refresh (qs);
  return G_SOURCE_CONTINUE;
}

void
sysmon_stop (QuickSettings *qs)
{
  if (qs->sysmon_timer != 0)
    {
      g_source_remove (qs->sysmon_timer);
      qs->sysmon_timer = 0;
    }
}

static void
on_sysmon_tile_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  sysmon_refresh (qs);
  if (qs->sysmon_timer == 0)
    qs->sysmon_timer = g_timeout_add_seconds (SYSMON_INTERVAL_S,
                                              sysmon_tick, qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "sysmon");
}

static void
on_sysmon_back_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  sysmon_stop (qs);
  show_main_page (qs);
}

static void
on_sysmon_popup_hide (GtkWidget *widget,
                      gpointer   user_data)
{
  (void) widget;
  sysmon_stop (user_data);
}

void
sysmon_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *sys_tile;
  GtkWidget *page;
  GtkWidget *content;
  GtkWidget *disks_grid;
  GtkWidget *back;

  /* Mini tile in the shared action grid. */
  sys_tile = make_mini_tile (box, "computer-symbolic", "System", NULL);
  g_signal_connect (sys_tile, "clicked",
                    G_CALLBACK (on_sysmon_tile_clicked), qs);

  /* Sub-page: RAM + CPU meter cards, then one card per disk. */
  page = make_sub_page ("System", &back);
  content = make_group (page, "Monitor");
  make_meter (content, 0, "Memory",
              &qs->sysmon_ram_value, &qs->sysmon_ram_bar);
  make_meter (content, 1, "Processor",
              &qs->sysmon_cpu_value, &qs->sysmon_cpu_bar);
  disks_grid = make_group (page, "Disks");
  qs->sysmon_disk_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  gtk_grid_attach (GTK_GRID (disks_grid), qs->sysmon_disk_box, 0, 0, 2, 1);
  gtk_stack_add_named (GTK_STACK (stack), page, "sysmon");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_sysmon_back_clicked), qs);
  g_signal_connect (qs->popup, "hide",
                    G_CALLBACK (on_sysmon_popup_hide), qs);
}
