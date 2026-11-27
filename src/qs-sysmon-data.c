#include "qs-sysmon-data.h"

#include <sys/statvfs.h>

/* Pseudo-filesystems never shown as disks. */
static const gchar *skip_fs[] = {
  "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "securityfs",
  "cgroup", "cgroup2", "pstore", "debugfs", "tracefs", "configfs",
  "fusectl", "mqueue", "hugetlbfs", "nsfs", "overlay", "squashfs", NULL
};

/* Remote/files de rede: statvfs pode travar na UI (servidor fora).
 * Nunca exibidos; pulados antes de qualquer statvfs. */
static gboolean
fs_is_network (const gchar *fstype,
               const gchar *device)
{
  if (fstype == NULL)
    return FALSE;
  if (g_str_has_prefix (fstype, "nfs"))
    return TRUE;
  if (g_strcmp0 (fstype, "cifs") == 0
      || g_str_has_prefix (fstype, "smb"))
    return TRUE;
  if (g_strcmp0 (fstype, "sshfs") == 0
      || g_str_has_prefix (fstype, "fuse.sshfs"))
    return TRUE;
  if (device != NULL)
    {
      if (g_str_has_prefix (device, "//"))
        return TRUE;
      if (strstr (device, ":/") != NULL)
        return TRUE;
    }
  return FALSE;
}

static gboolean
fs_skipped (const gchar *fstype,
            const gchar *device)
{
  gint i;

  for (i = 0; skip_fs[i] != NULL; i++)
    if (g_strcmp0 (fstype, skip_fs[i]) == 0)
      return TRUE;
  if (device != NULL && g_str_has_prefix (device, "/dev/loop"))
    return TRUE;
  if (fstype != NULL && g_str_has_prefix (fstype, "fuse.")
      && g_strcmp0 (fstype, "fuseblk") != 0)
    return TRUE;
  if (fs_is_network (fstype, device))
    return TRUE;
  return FALSE;
}

/* Seam de teste: override de caminho via env, default /proc.
 * QS_SYSMON_MOUNTS e QS_SYSMON_SWAPS permitem fixtures sem root. */
static const gchar *
sysmon_mounts_path (void)
{
  const gchar *p = g_getenv ("QS_SYSMON_MOUNTS");

  if (p != NULL && p[0] != '\0')
    return p;
  return "/proc/mounts";
}

static const gchar *
sysmon_swaps_path (void)
{
  const gchar *p = g_getenv ("QS_SYSMON_SWAPS");

  if (p != NULL && p[0] != '\0')
    return p;
  return "/proc/swaps";
}

/* DATA (sem widgets): read_memory/read_cpu/read_disks + helpers puros.
 * Nenhum GTK aqui. */

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

static gchar *
sysmon_size_text (guint64 bytes)
{
  if (bytes < 1073741824)
    return g_strdup_printf ("%.0f MB", (gdouble) bytes / 1048576.0);
  return g_strdup_printf ("%.2f GB", (gdouble) bytes / 1073741824.0);
}

gchar *
sysmon_bytes_text (guint64 bytes,
                   guint64 total)
{
  gchar *used = sysmon_size_text (bytes);
  gchar *whole = sysmon_size_text (total);
  gchar *out = g_strdup_printf ("%s / %s", used, whole);
  g_free (used);
  g_free (whole);
  return out;
}

/* /proc/mounts escapes space/tab/newline/backslash as octal. */
gchar *
unescape_mount (const gchar *s)
{
  GString *out = g_string_new (NULL);
  const gchar *p = s;

  while (*p != '\0')
    {
      if (p[0] == '\\' && p[1] == '1' && p[2] == '3' && p[3] == '4')
        {
          g_string_append_c (out, '\\');
          p += 4;
        }
      else if (p[0] == '\\' && p[1] == '0'
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

void
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
 * with sysmon_free_disks. Swap agrega /proc/swaps em uma linha "swap". */
GPtrArray *
sysmon_read_swaps (void)
{
  GPtrArray *swaps = g_ptr_array_new_with_free_func (disk_elem_free);
  gchar *content = NULL;
  gchar **lines;
  gint i;
  guint64 total_kb = 0;
  guint64 used_kb = 0;

  if (!g_file_get_contents (sysmon_swaps_path (), &content, NULL, NULL))
    return swaps;
  lines = g_strsplit (content, "\n", -1);
  g_free (content);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar **fields;
      gchar *tokens[8];
      gint ntok = 0;
      gint k;
      guint64 size_kb;
      guint64 used;

      if (lines[i][0] == '\0')
        continue;
      if (g_str_has_prefix (lines[i], "Filename"))
        continue;
      fields = g_strsplit_set (lines[i], " \t", -1);
      for (k = 0; fields[k] != NULL && ntok < 8; k++)
        {
          if (fields[k][0] == '\0')
            continue;
          tokens[ntok++] = fields[k];
        }
      if (ntok < 4)
        {
          g_strfreev (fields);
          continue;
        }
      size_kb = g_ascii_strtoull (tokens[2], NULL, 10);
      used = g_ascii_strtoull (tokens[3], NULL, 10);
      g_strfreev (fields);
      if (size_kb == 0)
        continue;
      if (used > size_kb)
        used = size_kb;
      total_kb += size_kb;
      used_kb += used;
    }
  g_strfreev (lines);
  if (total_kb > 0)
    {
      SysmonDisk *disk = g_new (SysmonDisk, 1);
      guint64 total = total_kb * 1024;
      guint64 used = used_kb * 1024;

      disk->name = g_strdup ("swap");
      disk->text = sysmon_bytes_text (used, total);
      disk->fraction = (total == 0) ? 0.0 : (gdouble) used / (gdouble) total;
      g_ptr_array_add (swaps, disk);
    }
  return swaps;
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

  if (!g_file_get_contents (sysmon_mounts_path (), &content, NULL, NULL))
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
        /* Usa f_bfree (nao f_bavail): f_bfree mede blocos fisicamente livres;
         * f_bavail esconde blocos reservados ao root e inflaria o uso visivel. */
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
  {
    GPtrArray *swaps = sysmon_read_swaps ();
    guint j;

    for (j = 0; j < swaps->len; j++)
      g_ptr_array_add (disks, g_ptr_array_index (swaps, j));
    g_ptr_array_set_free_func (swaps, NULL);
    g_ptr_array_free (swaps, TRUE);
  }
  return disks;
}

/* Chave "nome|" dos devices em /proc/mounts SEM statvfs (só parse,
 * barato e sem travar em FS remoto) mais assinatura de /proc/swaps.
 * Hash de montagens: quando igual ao qs->sysmon_disk_key, pula statvfs
 * por disco na UI. Inclui swaps para a linha swap nao congelar. */
gchar *
sysmon_mounts_key (void)
{
  gchar      *content = NULL;
  GString    *key;
  GHashTable *seen;
  gchar     **lines;
  gint        i;

  if (!g_file_get_contents (sysmon_mounts_path (), &content, NULL, NULL))
    return NULL;
  key = g_string_new (NULL);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  lines = g_strsplit (content, "\n", -1);
  g_free (content);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar **parts = g_strsplit (lines[i], " ", 4);
      gchar  *device;
      const gchar *base;

      if (g_strv_length (parts) < 3)
        {
          g_strfreev (parts);
          continue;
        }
      device = unescape_mount (parts[0]);
      if (fs_skipped (parts[2], device)
          || g_hash_table_contains (seen, device))
        {
          g_free (device);
          g_strfreev (parts);
          continue;
        }
      g_hash_table_add (seen, g_strdup (device));
      base = device;
      if (g_str_has_prefix (device, "/dev/"))
        base += strlen ("/dev/");
      g_string_append (key, base);
      g_string_append_c (key, '|');
      g_free (device);
      g_strfreev (parts);
    }
  g_strfreev (lines);
  g_hash_table_destroy (seen);
  {
    gchar *swap_content = NULL;
    gchar **swap_lines;
    gint k;

    if (g_file_get_contents (sysmon_swaps_path (), &swap_content, NULL, NULL))
      {
        swap_lines = g_strsplit (swap_content, "\n", -1);
        g_free (swap_content);
        for (k = 0; swap_lines[k] != NULL; k++)
          {
            gchar *s = g_strstrip (swap_lines[k]);

            if (s[0] == '\0')
              continue;
            if (g_str_has_prefix (s, "Filename"))
              continue;
            g_string_append (key, "swap:");
            g_string_append (key, s);
            g_string_append_c (key, '|');
          }
        g_strfreev (swap_lines);
      }
  }
  return g_string_free (key, FALSE);
}
