#include "qs-cache.h"
#include "qs-ui.h"
#include <sys/stat.h>

static gchar *
cache_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "xfce4",
                           "quick-settings", "state.ini", NULL);
}

static gboolean
fresh_since (gint64 now, gint64 ts, gint64 ttl)
{
  if (ts <= 0 || ts > now)
    return FALSE;
  return (now - ts) <= ttl;
}

static gboolean
read_bool (GKeyFile *kf, const gchar *group, const gchar *key, gboolean *out)
{
  GError *err = NULL;
  gboolean v = g_key_file_get_boolean (kf, group, key, &err);

  if (err != NULL)
    {
      g_error_free (err);
      return FALSE;
    }
  *out = v;
  return TRUE;
}

static gboolean
read_int (GKeyFile *kf, const gchar *group, const gchar *key, gint *out)
{
  GError *err = NULL;
  gint v = g_key_file_get_integer (kf, group, key, &err);

  if (err != NULL)
    {
      g_error_free (err);
      return FALSE;
    }
  *out = v;
  return TRUE;
}

static gboolean
read_ts (GKeyFile *kf, const gchar *group, const gchar *key, gint64 *out)
{
  GError *err = NULL;
  gint64 v = g_key_file_get_int64 (kf, group, key, &err);

  if (err != NULL)
    {
      g_error_free (err);
      return FALSE;
    }
  *out = v;
  return TRUE;
}

static void
write_keyfile (GKeyFile *kf, const gchar *path)
{
  gchar *data = g_key_file_to_data (kf, NULL, NULL);
  gchar *dir;
  GError *err = NULL;

  if (data == NULL)
    return;
  dir = g_path_get_dirname (path);
  if (dir != NULL)
    {
      g_mkdir_with_parents (dir, 0700);
      g_free (dir);
    }
  if (g_file_set_contents (path, data, -1, &err))
    chmod (path, 0600);
  else
    g_clear_error (&err);
  g_free (data);
}

static void
ensure_present_bool (GKeyFile *kf, const gchar *group, const gchar *key,
                     gboolean fallback)
{
  if (!g_key_file_has_key (kf, group, key, NULL))
    g_key_file_set_boolean (kf, group, key, fallback);
}

static void
ensure_present_int (GKeyFile *kf, const gchar *group, const gchar *key,
                    gint fallback)
{
  if (!g_key_file_has_key (kf, group, key, NULL))
    g_key_file_set_integer (kf, group, key, fallback);
}

static void
ensure_present_ts (GKeyFile *kf, const gchar *group, const gchar *key)
{
  if (!g_key_file_has_key (kf, group, key, NULL))
    g_key_file_set_int64 (kf, group, key, 0);
}

static void
ensure_present_str (GKeyFile *kf, const gchar *group, const gchar *key)
{
  if (!g_key_file_has_key (kf, group, key, NULL))
    g_key_file_set_string (kf, group, key, "");
}

void
qs_cache_save (QuickSettings *qs)
{
  GKeyFile *kf;
  gchar *path;
  gint64 now;

  if (qs == NULL)
    return;
  path = cache_path ();
  kf = g_key_file_new ();
  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);
  now = g_get_real_time () / G_USEC_PER_SEC;

  g_key_file_set_int64 (kf, "meta", "saved_at", now);

  g_key_file_set_boolean (kf, "wifi", "on", qs->wifi_on);
  g_key_file_set_int64 (kf, "wifi", "on_ts", now);
  if (qs->wifi_on && qs->wifi_ssid != NULL && qs->wifi_ssid[0] != '\0')
    {
      g_key_file_set_string (kf, "wifi", "ssid", qs->wifi_ssid);
      g_key_file_set_int64 (kf, "wifi", "ssid_ts", now);
    }
  else
    {
      ensure_present_str (kf, "wifi", "ssid");
      ensure_present_ts (kf, "wifi", "ssid_ts");
    }

  g_key_file_set_boolean (kf, "bluetooth", "powered", qs->bt_on);
  g_key_file_set_int64 (kf, "bluetooth", "powered_ts", now);
  ensure_present_bool (kf, "bluetooth", "connected", FALSE);
  if (qs->bt_on && qs->bt_connected_name != NULL && qs->bt_connected_name[0] != '\0')
    {
      g_key_file_set_string (kf, "bluetooth", "connected_name",
                              qs->bt_connected_name);
      g_key_file_set_int64 (kf, "bluetooth", "connected_name_ts", now);
    }
  else
    {
      ensure_present_str (kf, "bluetooth", "connected_name");
      ensure_present_ts (kf, "bluetooth", "connected_name_ts");
    }
  ensure_present_ts (kf, "bluetooth", "connected_ts");

  ensure_present_int (kf, "audio", "volume", 50);
  ensure_present_bool (kf, "audio", "mute", FALSE);
  ensure_present_ts (kf, "audio", "mute_ts");

  ensure_present_int (kf, "display", "brightness", 50);
  ensure_present_ts (kf, "display", "brightness_ts");

  ensure_present_int (kf, "power", "percent", 100);
  ensure_present_ts (kf, "power", "percent_ts");

  ensure_present_int (kf, "notify", "count", 0);
  ensure_present_ts (kf, "notify", "count_ts");

  ensure_present_str (kf, "theme", "name");
  ensure_present_bool (kf, "nightlight", "enabled", FALSE);

  write_keyfile (kf, path);
  g_key_file_free (kf);
  g_free (path);
}

static guint pending_id = 0;
static QuickSettings *pending_qs = NULL;

static gboolean
save_tick (gpointer data)
{
  QuickSettings *qs = data;

  pending_id = 0;
  pending_qs = NULL;
  qs_cache_save (qs);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

void
qs_cache_schedule_save (QuickSettings *qs)
{
  if (qs == NULL || pending_id != 0)
    return;
  qs_ref (qs);
  pending_qs = qs;
  pending_id = g_timeout_add (QS_CACHE_DEBOUNCE_MS, save_tick, qs);
}

void
qs_cache_stop (void)
{
  if (pending_id != 0)
    {
      g_source_remove (pending_id);
      pending_id = 0;
    }
  if (pending_qs != NULL)
    {
      QuickSettings *qs = pending_qs;

      pending_qs = NULL;
      qs_unref (qs);
    }
}

void
qs_cache_load (QuickSettings *qs)
{
  GKeyFile *kf;
  gchar *path;
  gint64 now;
  gboolean on = FALSE, powered = FALSE, connected = FALSE, mute = FALSE;
  gboolean enabled = FALSE;
  gint volume = 0, brightness = 0, percent = 0, count = 0;
  gint64 ts = 0;
  gchar *str = NULL;
  GError *err = NULL;
  gboolean wifi_text_set = FALSE;

  if (qs == NULL || g_atomic_int_get (&qs->dead))
    return;
  path = cache_path ();
  kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    {
      g_key_file_free (kf);
      g_free (path);
      return;
    }
  g_free (path);
  now = g_get_real_time () / G_USEC_PER_SEC;

  if (read_bool (kf, "wifi", "on", &on)
      && read_ts (kf, "wifi", "on_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_SLOW_S))
    {
      qs->wifi_on = on;
      if (qs->wifi_switch != NULL)
        {
          qs->updating = TRUE;
          gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), on);
          qs->updating = FALSE;
        }
      if (qs->wifi_tile != NULL)
        {
          set_tile_active (qs->wifi_tile, on);
          gtk_widget_set_sensitive (qs->wifi_tile, TRUE);
        }
      if (qs->wifi_sub != NULL)
        gtk_label_set_text (GTK_LABEL (qs->wifi_sub), on ? "On" : "Off");
      wifi_text_set = TRUE;
    }
  else if (qs->wifi_sub != NULL && !wifi_text_set)
    gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "…");

  str = g_key_file_get_string (kf, "wifi", "ssid", &err);
  if (err != NULL)
    {
      g_clear_error (&err);
      g_free (str);
      str = NULL;
    }
  if (str != NULL && *str != '\0'
      && read_ts (kf, "wifi", "ssid_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_FAST_S)
      && qs->wifi_sub != NULL)
    {
      gtk_label_set_text (GTK_LABEL (qs->wifi_sub), str);
      g_free (qs->wifi_ssid);
      qs->wifi_ssid = g_strdup (str);
    }
  g_free (str);
  str = NULL;

  if (read_bool (kf, "bluetooth", "powered", &powered)
      && read_ts (kf, "bluetooth", "powered_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_SLOW_S))
    {
      qs->bt_on = powered;
      if (qs->bt_switch != NULL)
        {
          qs->updating = TRUE;
          gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), powered);
          qs->updating = FALSE;
        }
      if (qs->bt_tile != NULL)
        set_tile_active (qs->bt_tile, powered);
      if (qs->bt_sub != NULL)
        gtk_label_set_text (GTK_LABEL (qs->bt_sub), powered ? "On" : "Off");
    }
  else if (qs->bt_sub != NULL)
    gtk_label_set_text (GTK_LABEL (qs->bt_sub), "…");

  str = g_key_file_get_string (kf, "bluetooth", "connected_name", &err);
  if (err != NULL)
    {
      g_clear_error (&err);
      g_free (str);
      str = NULL;
    }
  if (str != NULL && *str != '\0'
      && read_ts (kf, "bluetooth", "connected_name_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_FAST_S)
      && qs->bt_sub != NULL)
    {
      g_free (qs->bt_connected_name);
      qs->bt_connected_name = g_strdup (str);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), str);
    }
  else if (read_bool (kf, "bluetooth", "connected", &connected)
           && read_ts (kf, "bluetooth", "connected_ts", &ts)
           && fresh_since (now, ts, QS_CACHE_TTL_FAST_S)
           && connected && qs->bt_sub != NULL)
    gtk_label_set_text (GTK_LABEL (qs->bt_sub), "Connected");
  g_free (str);
  str = NULL;

  if (read_int (kf, "audio", "volume", &volume) && qs->vol_scale != NULL)
    {
      qs->updating = TRUE;
      gtk_range_set_value (GTK_RANGE (qs->vol_scale),
                           (gdouble) CLAMP (volume, 0, 100));
      qs->updating = FALSE;
    }

  if (read_bool (kf, "audio", "mute", &mute)
      && read_ts (kf, "audio", "mute_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_FAST_S)
      && mute && qs->audio_sub != NULL)
    gtk_label_set_text (GTK_LABEL (qs->audio_sub), "Muted");

  if (read_int (kf, "display", "brightness", &brightness)
      && read_ts (kf, "display", "brightness_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_SLOW_S)
      && qs->bright_scale != NULL)
    {
      qs->updating = TRUE;
      gtk_range_set_value (GTK_RANGE (qs->bright_scale),
                           (gdouble) CLAMP (brightness, 0, 100));
      qs->updating = FALSE;
    }

  if (read_int (kf, "power", "percent", &percent)
      && read_ts (kf, "power", "percent_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_SLOW_S)
      && qs->bat_label != NULL)
    {
      gchar *text = g_strdup_printf ("%d%%", CLAMP (percent, 0, 100));

      gtk_label_set_text (GTK_LABEL (qs->bat_label), text);
      g_free (text);
    }
  else if (qs->bat_label != NULL)
    gtk_label_set_text (GTK_LABEL (qs->bat_label), "—");

  if (read_int (kf, "notify", "count", &count)
      && read_ts (kf, "notify", "count_ts", &ts)
      && fresh_since (now, ts, QS_CACHE_TTL_SLOW_S)
      && qs->notify_sub != NULL)
    {
      if (count <= 0)
        gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
      else
        {
          gchar *text = g_strdup_printf ("%d", count);

          gtk_label_set_text (GTK_LABEL (qs->notify_sub), text);
          g_free (text);
        }
    }
  else if (qs->notify_sub != NULL)
    gtk_label_set_text (GTK_LABEL (qs->notify_sub), "…");

  str = g_key_file_get_string (kf, "theme", "name", &err);
  if (err != NULL)
    g_clear_error (&err);
  g_free (str);
  str = NULL;

  if (read_bool (kf, "nightlight", "enabled", &enabled))
    {
      if (qs->night_tile != NULL)
        set_tile_active (qs->night_tile, enabled);
      if (qs->night_sub != NULL)
        gtk_label_set_text (GTK_LABEL (qs->night_sub),
                            enabled ? "On" : "Off");
    }

  g_key_file_free (kf);
}
