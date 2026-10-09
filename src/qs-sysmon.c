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
bytes_text (guint64 bytes,
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

  /* RAM + CPU: update the value labels in place (no widget churn). */
  if (read_meminfo (&total_kb, &avail_kb) && total_kb > 0)
    {
      used_bytes = (total_kb - MIN (avail_kb, total_kb)) * 1024;
      total_bytes = total_kb * 1024;
      text = bytes_text (used_bytes, total_bytes);
      gtk_label_set_text (GTK_LABEL (qs->sysmon_ram_value), text);
      g_free (text);
    }
  cpu = read_cpu_percent (qs);
  if (cpu >= 0.0)
    {
      text = g_strdup_printf ("%d%%", (gint) (cpu + 0.5));
      gtk_label_set_text (GTK_LABEL (qs->sysmon_cpu_value), text);
      g_free (text);
    }

  /* Disks: gather the data first, then only touch widgets when the
   * device set changed; otherwise update the value labels in place. */
  {
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    GPtrArray *texts = g_ptr_array_new_with_free_func (g_free);
    GString   *key = g_string_new (NULL);

    if (g_file_get_contents ("/proc/mounts", &content, NULL, NULL))
      {
        /* One row per device: the same partition often appears under
         * several mount points (bind mounts, btrfs subvolumes). */
        GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);

        lines = g_strsplit (content, "\n", -1);
        g_free (content);
        content = NULL;
        for (i = 0; lines[i] != NULL; i++)
          {
            gchar **parts = g_strsplit (lines[i], " ", 4);
            struct statvfs st;
            guint64 total;
            guint64 used;
            gchar *device;
            gchar *target;
            const gchar *base;

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
            g_string_append (key, device);
            g_string_append_c (key, '|');
            g_ptr_array_add (names, g_strdup (base));
            g_ptr_array_add (texts, bytes_text (used, total));
            g_free (device);
            g_free (target);
            g_strfreev (parts);
          }
        g_strfreev (lines);
        g_hash_table_destroy (seen);
      }

    if (g_strcmp0 (key->str, qs->sysmon_disk_key) == 0)
      {
        /* Same devices: refresh the values in place. */
        GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->sysmon_disk_box));
        GList *l;
        guint  idx = 0;

        for (l = rows; l != NULL && idx < texts->len; l = l->next, idx++)
          {
            GList     *cols = gtk_container_get_children (GTK_CONTAINER (l->data));
            GtkWidget *value = GTK_WIDGET (g_list_last (cols)->data);

            if (GTK_IS_LABEL (value))
              gtk_label_set_text (GTK_LABEL (value),
                                  g_ptr_array_index (texts, idx));
            g_list_free (cols);
          }
        g_list_free (rows);
        g_string_free (key, TRUE);
      }
    else
      {
        guint idx;

        clear_list_box (qs->sysmon_disk_box);
        if (names->len == 0)
          {
            GtkWidget *empty = gtk_label_new ("No disks found");

            ui_add_class (empty, "qs-status");
            gtk_widget_set_halign (empty, GTK_ALIGN_START);
            gtk_box_pack_start (GTK_BOX (qs->sysmon_disk_box), empty,
                                FALSE, FALSE, 0);
          }
        else
          {
            for (idx = 0; idx < names->len; idx++)
              {
                GtkWidget *value = NULL;

                make_stat_row (qs->sysmon_disk_box,
                               g_ptr_array_index (names, idx), &value);
                gtk_label_set_text (GTK_LABEL (value),
                                    g_ptr_array_index (texts, idx));
              }
          }
        g_free (qs->sysmon_disk_key);
        qs->sysmon_disk_key = g_string_free (key, FALSE);
        gtk_widget_show_all (qs->sysmon_disk_box);
      }
    g_ptr_array_free (names, TRUE);
    g_ptr_array_free (texts, TRUE);
  }
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
  GtkWidget *monitor_card;
  GtkWidget *disks_grid;
  GtkWidget *back;

  /* Mini tile in the shared action grid. */
  sys_tile = make_mini_tile (box, "computer-symbolic", "System", NULL);
  g_signal_connect (sys_tile, "clicked",
                    G_CALLBACK (on_sysmon_tile_clicked), qs);

  /* Sub-page: one stat card for Monitor, one for Disks. Values are
   * plain labels updated in place (no progress bars). */
  page = make_sub_page ("System", &back);
  content = make_group (page, "Monitor");
  monitor_card = make_stat_card (content, 0);
  make_stat_row (monitor_card, "Memory", &qs->sysmon_ram_value);
  make_stat_row (monitor_card, "Processor", &qs->sysmon_cpu_value);
  disks_grid = make_group (page, "Disks");
  qs->sysmon_disk_box = make_stat_card (disks_grid, 0);
  gtk_stack_add_named (GTK_STACK (stack), page, "sysmon");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_sysmon_back_clicked), qs);
  g_signal_connect (qs->popup, "hide",
                    G_CALLBACK (on_sysmon_popup_hide), qs);
}
