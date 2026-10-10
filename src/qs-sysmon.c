#include "qs-sysmon.h"
#include "qs-ui.h"

#include <sys/statvfs.h>
#include <unistd.h>

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

/* Compact "used/total" for row headers; the percent lives on the bar. */
gchar *
sysmon_bytes_text (guint64 bytes,
                   guint64 total)
{
  return g_strdup_printf ("%.1f/%.1f GB",
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
        disk->fraction = (total == 0) ? 0.0
                                      : (gdouble) used / (gdouble) total;
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

/* Resource row: header (title + compact value) over a bar row
 * [expandable bar + % label at right]. The % lives in its own label
 * (the bar never shows overlay text). Value/bar/pct widgets are
 * returned for in-place refresh and also stashed on the row
 * ("qs-sys-value"/"qs-sys-bar"/"qs-sys-pct") so disk rows can be
 * updated without tracking extra state. */
static GtkWidget *
make_sys_card (GtkWidget   *box,
               const gchar *title,
               GtkWidget  **value_out,
               GtkWidget  **bar_out,
               GtkWidget  **pct_out)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *name = gtk_label_new (title);
  GtkWidget *value = gtk_label_new ("—");
  GtkWidget *brow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *bar = gtk_progress_bar_new ();
  GtkWidget *pct = gtk_label_new ("—");

  ui_add_class (name, "qs-title");
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  ui_add_class (value, "qs-sys-num");
  gtk_widget_set_halign (value, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (head), name, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (head), value, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (card), head, FALSE, FALSE, 0);
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (bar), FALSE);
  gtk_widget_set_hexpand (bar, TRUE);
  gtk_widget_set_halign (bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
  ui_add_class (pct, "qs-sys-num");
  gtk_label_set_width_chars (GTK_LABEL (pct), 4);
  gtk_label_set_xalign (GTK_LABEL (pct), 1.0);
  gtk_widget_set_halign (pct, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (brow), bar, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (brow), pct, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (card), brow, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), card, FALSE, FALSE, 0);
  g_object_set_data (G_OBJECT (card), "qs-sys-value", value);
  g_object_set_data (G_OBJECT (card), "qs-sys-bar", bar);
  g_object_set_data (G_OBJECT (card), "qs-sys-pct", pct);

  if (value_out != NULL)
    *value_out = value;
  if (bar_out != NULL)
    *bar_out = bar;
  if (pct_out != NULL)
    *pct_out = pct;
  return card;
}

/* Sets bar fraction + right-side % label from a 0-1 fraction. */
static void
sysmon_set_bar (GtkWidget *bar,
                GtkWidget *pct,
                gdouble    fraction)
{
  gchar *text;

  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), fraction);
  text = g_strdup_printf ("%d%%", (gint) (fraction * 100.0 + 0.5));
  gtk_label_set_text (GTK_LABEL (pct), text);
  g_free (text);
}

/* Horizontal <hr> between rows (never before first / after last). */
static void
sysmon_add_separator (GtkWidget *box)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  ui_add_class (sep, "qs-sep");
  gtk_widget_set_margin_top (sep, QS_INNER_GAP);
  gtk_widget_set_margin_bottom (sep, QS_INNER_GAP);
  gtk_box_pack_start (GTK_BOX (box), sep, FALSE, FALSE, 0);
}

/* Distinct (physical id, core id) keys for qsort-style dedup. */
static gint
cpu_pair_compare (gconstpointer a,
                  gconstpointer b)
{
  guint64 ua = *(const guint64 *) a;
  guint64 ub = *(const guint64 *) b;

  if (ua < ub)
    return -1;
  if (ua > ub)
    return 1;
  return 0;
}

/* CPU topology ("6C/12T") for the CPU row header value. Physical cores
 * come from distinct (physical id, core id) pairs in /proc/cpuinfo,
 * threads from sysconf. Computed once and cached: topology never
 * changes at runtime. Falls back to "12T" when cores can't be told
 * apart (VMs, ARM without those fields). */
static const gchar *
sysmon_cpu_topology_text (void)
{
  static gchar *cached = NULL;
  glong threads;
  gint cores = 0;

  if (cached != NULL)
    return cached;
  threads = sysconf (_SC_NPROCESSORS_ONLN);
  if (threads <= 0)
    threads = sysconf (_SC_NPROCESSORS_CONF);
  if (threads <= 0)
    threads = 1;
  {
    gchar *content = NULL;

    if (g_file_get_contents ("/proc/cpuinfo", &content, NULL, NULL))
      {
        gchar **lines = g_strsplit (content, "\n", -1);
        GArray *pairs = g_array_new (FALSE, FALSE, sizeof (guint64));
        gint phys = -1;
        gint core = -1;
        gint i;

        g_free (content);
        for (i = 0; lines[i] != NULL; i++)
          {
            if (g_str_has_prefix (lines[i], "physical id"))
              {
                const gchar *colon = strchr (lines[i], ':');

                if (colon != NULL)
                  phys = atoi (colon + 1);
              }
            else if (g_str_has_prefix (lines[i], "core id"))
              {
                const gchar *colon = strchr (lines[i], ':');

                if (colon != NULL)
                  core = atoi (colon + 1);
              }
            else if (lines[i][0] == '\0')
              {
                if (phys >= 0 && core >= 0)
                  {
                    guint64 key = ((guint64) (guint32) phys << 32)
                                  | (guint32) core;

                    g_array_append_val (pairs, key);
                  }
                phys = -1;
                core = -1;
              }
          }
        if (phys >= 0 && core >= 0)
          {
            guint64 key = ((guint64) (guint32) phys << 32) | (guint32) core;

            g_array_append_val (pairs, key);
          }
        if (pairs->len > 0)
          {
            guint j;

            g_array_sort (pairs, cpu_pair_compare);
            cores = 1;
            for (j = 1; j < pairs->len; j++)
              if (g_array_index (pairs, guint64, j)
                  != g_array_index (pairs, guint64, j - 1))
                cores++;
          }
        g_array_free (pairs, TRUE);
        g_strfreev (lines);
      }
  }
  if (cores > 0 && cores <= threads)
    cached = g_strdup_printf ("%dC/%ldT", cores, (glong) threads);
  else
    cached = g_strdup_printf ("%ldT", (glong) threads);
  return cached;
}

static void
sysmon_refresh (QuickSettings *qs)
{
  guint64 total_kb = 0;
  guint64 avail_kb = 0;
  gdouble cpu;
  gchar  *text;

  if (sysmon_read_memory (&total_kb, &avail_kb) && total_kb > 0)
    {
      guint64 used = (total_kb - MIN (avail_kb, total_kb)) * 1024;
      guint64 total = total_kb * 1024;
      gdouble frac = (gdouble) used / (gdouble) total;

      text = g_strdup_printf ("%.1f/%.1f GB",
                              used / 1073741824.0, total / 1073741824.0);
      gtk_label_set_text (GTK_LABEL (qs->sysmon_ram_value), text);
      g_free (text);
      sysmon_set_bar (qs->sysmon_ram_bar, qs->sysmon_ram_pct, frac);
    }
  cpu = sysmon_read_cpu (qs);
  if (cpu >= 0.0)
    {
      /* Header value shows static topology; the side label keeps live %. */
      gtk_label_set_text (GTK_LABEL (qs->sysmon_cpu_value),
                          sysmon_cpu_topology_text ());
      sysmon_set_bar (qs->sysmon_cpu_bar, qs->sysmon_cpu_pct, cpu / 100.0);
    }

  /* Disks: rebuild the blocks only when the device set changed,
   * otherwise update the values in place. Separators are children of
   * the same box, so the in-place path skips non-row children and the
   * rebuild path recreates exactly N-1 separators (clear wipes them). */
  {
    GPtrArray *disks = sysmon_read_disks ();
    GString   *key = g_string_new (NULL);
    guint      i;

    for (i = 0; i < disks->len; i++)
      {
        SysmonDisk *d = g_ptr_array_index (disks, i);

        g_string_append (key, d->name);
        g_string_append_c (key, '|');
      }
    if (g_strcmp0 (key->str, qs->sysmon_disk_key) == 0)
      {
        GList *cards = gtk_container_get_children (GTK_CONTAINER (qs->sysmon_disk_box));
        GList *l;
        guint  idx = 0;

        for (l = cards; l != NULL && idx < disks->len; l = l->next)
          {
            SysmonDisk *d;
            GtkWidget  *value;
            GtkWidget  *bar;
            GtkWidget  *pct;

            if (g_object_get_data (G_OBJECT (l->data), "qs-sys-bar") == NULL)
              continue;
            d = g_ptr_array_index (disks, idx);
            value = g_object_get_data (G_OBJECT (l->data),
                                       "qs-sys-value");
            bar = g_object_get_data (G_OBJECT (l->data),
                                     "qs-sys-bar");
            pct = g_object_get_data (G_OBJECT (l->data),
                                     "qs-sys-pct");

            if (value != NULL && bar != NULL && pct != NULL
                && GTK_IS_LABEL (value) && GTK_IS_PROGRESS_BAR (bar)
                && GTK_IS_LABEL (pct))
              {
                gtk_label_set_text (GTK_LABEL (value), d->text);
                sysmon_set_bar (bar, pct, d->fraction);
              }
            idx++;
          }
        g_list_free (cards);
        g_string_free (key, TRUE);
      }
    else
      {
        clear_list_box (qs->sysmon_disk_box);
        if (disks->len == 0)
          {
            GtkWidget *empty = gtk_label_new ("No disks found");

            ui_add_class (empty, "qs-status");
            gtk_widget_set_halign (empty, GTK_ALIGN_START);
            gtk_box_pack_start (GTK_BOX (qs->sysmon_disk_box), empty,
                                FALSE, FALSE, 0);
          }
        else
          {
            for (i = 0; i < disks->len; i++)
              {
                SysmonDisk *d = g_ptr_array_index (disks, i);
                GtkWidget  *value = NULL;
                GtkWidget  *bar = NULL;
                GtkWidget  *pct = NULL;

                if (i > 0)
                  sysmon_add_separator (qs->sysmon_disk_box);
                make_sys_card (qs->sysmon_disk_box,
                               d->name, &value, &bar, &pct);
                gtk_label_set_text (GTK_LABEL (value), d->text);
                sysmon_set_bar (bar, pct, d->fraction);
              }
          }
        g_free (qs->sysmon_disk_key);
        qs->sysmon_disk_key = g_string_free (key, FALSE);
        gtk_widget_show_all (qs->sysmon_disk_box);
      }
    sysmon_free_disks (disks);
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
  GtkWidget *disks_grid;
  GtkWidget *back;

  /* Mini tile in the shared action grid. */
  sys_tile = make_mini_tile (box, "computer-symbolic", "System", NULL);
  g_signal_connect (sys_tile, "clicked",
                    G_CALLBACK (on_sysmon_tile_clicked), qs);

  /* One row per resource: header (title + compact value) + bar row
   * [bar + % label]. Memory/CPU rows are fixed; disks get one row
   * each under Disks. Separators sit between rows only (never before
   * the first nor after the last). */
  page = make_sub_page ("System", &back);
  make_sys_card (page, "Memory",
                 &qs->sysmon_ram_value, &qs->sysmon_ram_bar,
                 &qs->sysmon_ram_pct);
  sysmon_add_separator (page);
  make_sys_card (page, "CPU",
                 &qs->sysmon_cpu_value, &qs->sysmon_cpu_bar,
                 &qs->sysmon_cpu_pct);
  sysmon_add_separator (page);
  disks_grid = make_group (page, "Disks");
  qs->sysmon_disk_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_SECTION_GAP);
  gtk_grid_attach (GTK_GRID (disks_grid), qs->sysmon_disk_box, 0, 0, 2, 1);
  gtk_stack_add_named (GTK_STACK (stack), page, "sysmon");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_sysmon_back_clicked), qs);
  g_signal_connect (qs->popup, "hide",
                    G_CALLBACK (on_sysmon_popup_hide), qs);
}
