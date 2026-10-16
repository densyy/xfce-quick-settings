/* Testes do qs-cache: roundtrip save/load, TTL, debounce, permissões.
 *
 * Isolamento: XDG_CACHE_HOME temporário (setado 1x no main — o GLib
 * cacheia o valor); o ini é removido entre os testes.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sys/stat.h>

#include "qs-core.h"
#include "qs-cache.h"

static gboolean  have_display = FALSE;
static gchar    *tmp_root = NULL;

static QuickSettings *
new_qs (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs->refcount = 1;
  return qs;
}

static void
free_qs (QuickSettings *qs)
{
  qs_unref (qs);
}

static gchar *
ini_path (void)
{
  return g_build_filename (tmp_root, "xfce4", "quick-settings",
                           "state.ini", NULL);
}

static void
rm_ini (void)
{
  gchar *path = ini_path ();

  g_remove (path);
  g_free (path);
}

static void
write_ini (GKeyFile *kf)
{
  gchar *path = ini_path ();
  gchar *dir = g_path_get_dirname (path);
  gchar *data = NULL;
  gsize  len = 0;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  data = g_key_file_to_data (kf, &len, NULL);
  g_assert_nonnull (data);
  g_assert_true (g_file_set_contents (path, data, (gssize) len, NULL));
  g_free (data);
  g_free (dir);
  g_free (path);
}

/* save grava wifi on/ssid + bluetooth powered quando ligados. */
static void
test_save_wifi_bt (void)
{
  QuickSettings *qs = new_qs ();
  GKeyFile      *kf = g_key_file_new ();
  gchar         *path = ini_path ();

  rm_ini ();
  qs->wifi_on = TRUE;
  g_free (qs->wifi_ssid);
  qs->wifi_ssid = g_strdup ("Casa");
  qs->bt_on = TRUE;
  qs_cache_save (qs);
  g_assert_true (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL));
  g_assert_true (g_key_file_get_boolean (kf, "wifi", "on", NULL));
  g_assert_cmpstr (g_key_file_get_string (kf, "wifi", "ssid", NULL), ==, "Casa");
  g_assert_true (g_key_file_get_boolean (kf, "bluetooth", "powered", NULL));
  g_key_file_free (kf);
  g_free (path);
  free_qs (qs);
}

/* save sempre escreve defaults (audio/display/power/notify/theme/nightlight). */
static void
test_save_defaults (void)
{
  QuickSettings *qs = new_qs ();
  GKeyFile      *kf = g_key_file_new ();
  gchar         *path = ini_path ();

  rm_ini ();
  qs_cache_save (qs);
  g_assert_true (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL));
  g_assert_cmpint (g_key_file_get_integer (kf, "audio", "volume", NULL), ==, 50);
  g_assert_false (g_key_file_get_boolean (kf, "audio", "mute", NULL));
  g_assert_cmpint (g_key_file_get_integer (kf, "display", "brightness", NULL), ==, 50);
  g_assert_cmpint (g_key_file_get_integer (kf, "power", "percent", NULL), ==, 100);
  g_assert_cmpint (g_key_file_get_integer (kf, "notify", "count", NULL), ==, 0);
  g_assert_false (g_key_file_get_boolean (kf, "bluetooth", "connected", NULL));
  g_key_file_free (kf);
  g_free (path);
  free_qs (qs);
}

/* load aplica wifi/audio/display/power/notify nos widgets (dentro do TTL). */
static void
test_load_widgets (void)
{
  QuickSettings *qs = new_qs ();
  GKeyFile      *kf = g_key_file_new ();
  gint64         now = g_get_real_time () / G_USEC_PER_SEC;

  if (!have_display)
    {
      g_test_skip ("needs display");
      g_key_file_free (kf);
      free_qs (qs);
      return;
    }
  qs->wifi_sub = gtk_label_new ("");
  qs->wifi_switch = gtk_switch_new ();
  qs->vol_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  qs->audio_sub = gtk_label_new ("");
  qs->bright_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  qs->bat_label = gtk_label_new ("");
  qs->notify_sub = gtk_label_new ("");

  g_key_file_set_boolean (kf, "wifi", "on", TRUE);
  g_key_file_set_int64 (kf, "wifi", "on_ts", now);
  g_key_file_set_string (kf, "wifi", "ssid", "Casa");
  g_key_file_set_int64 (kf, "wifi", "ssid_ts", now);
  g_key_file_set_integer (kf, "audio", "volume", 30);
  g_key_file_set_boolean (kf, "audio", "mute", TRUE);
  g_key_file_set_int64 (kf, "audio", "mute_ts", now);
  g_key_file_set_integer (kf, "display", "brightness", 70);
  g_key_file_set_int64 (kf, "display", "brightness_ts", now);
  g_key_file_set_integer (kf, "power", "percent", 55);
  g_key_file_set_int64 (kf, "power", "percent_ts", now);
  g_key_file_set_integer (kf, "notify", "count", 7);
  g_key_file_set_int64 (kf, "notify", "count_ts", now);
  write_ini (kf);
  g_key_file_free (kf);

  qs_cache_load (qs);
  g_assert_true (qs->wifi_on);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (qs->wifi_switch)));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->wifi_sub)), ==, "Casa");
  g_assert_cmpstr (qs->wifi_ssid, ==, "Casa");
  g_assert_cmpfloat (gtk_range_get_value (GTK_RANGE (qs->vol_scale)), ==, 30.0);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->audio_sub)), ==, "Muted");
  g_assert_cmpfloat (gtk_range_get_value (GTK_RANGE (qs->bright_scale)), ==, 70.0);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bat_label)), ==, "55%");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "7");
  free_qs (qs);
}

/* load ignora entradas com ts fora do TTL. */
static void
test_load_stale (void)
{
  QuickSettings *qs = new_qs ();
  GKeyFile      *kf = g_key_file_new ();
  gint64         now = g_get_real_time () / G_USEC_PER_SEC;
  gint64         old = now - 3600;

  qs->wifi_on = TRUE;
  g_free (qs->wifi_ssid);
  qs->wifi_ssid = g_strdup ("Old");
  g_key_file_set_boolean (kf, "wifi", "on", FALSE);
  g_key_file_set_int64 (kf, "wifi", "on_ts", old);
  g_key_file_set_string (kf, "wifi", "ssid", "New");
  g_key_file_set_int64 (kf, "wifi", "ssid_ts", old);
  write_ini (kf);
  g_key_file_free (kf);

  qs_cache_load (qs);
  g_assert_true (qs->wifi_on);
  g_assert_cmpstr (qs->wifi_ssid, ==, "Old");
  free_qs (qs);
}

/* load com dead=TRUE é no-op. */
static void
test_load_dead (void)
{
  QuickSettings *qs = new_qs ();
  GKeyFile      *kf = g_key_file_new ();
  gint64         now = g_get_real_time () / G_USEC_PER_SEC;

  g_key_file_set_boolean (kf, "wifi", "on", TRUE);
  g_key_file_set_int64 (kf, "wifi", "on_ts", now);
  write_ini (kf);
  g_key_file_free (kf);

  g_atomic_int_set (&qs->dead, TRUE);
  qs_cache_load (qs);
  g_assert_false (qs->wifi_on);
  free_qs (qs);
}

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

/* schedule + stop imediato: nada é escrito. */
static void
test_schedule_stop (void)
{
  QuickSettings *qs = new_qs ();
  GMainLoop     *loop = g_main_loop_new (NULL, FALSE);
  gchar         *path = ini_path ();

  rm_ini ();
  qs_cache_schedule_save (qs);
  qs_cache_stop ();
  g_timeout_add (700, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  g_free (path);
  free_qs (qs);
}

/* schedule sem stop: o debounce (500ms) dispara o save. */
static void
test_schedule_fires (void)
{
  QuickSettings *qs = new_qs ();
  GMainLoop     *loop = g_main_loop_new (NULL, FALSE);
  gchar         *path = ini_path ();

  rm_ini ();
  qs_cache_schedule_save (qs);
  g_timeout_add (800, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
  g_free (path);
  qs_cache_stop ();
  free_qs (qs);
}

/* ini corrompido: load não crasha e não aplica nada. */
static void
test_corrupt_ini (void)
{
  QuickSettings *qs = new_qs ();
  gchar         *path = ini_path ();
  gchar         *dir = g_path_get_dirname (path);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, "[[[not ini", -1, NULL));
  qs_cache_load (qs);
  g_assert_false (qs->wifi_on);
  g_free (dir);
  g_free (path);
  free_qs (qs);
}

/* Guards de NULL não crasham. */
static void
test_null_guards (void)
{
  qs_cache_save (NULL);
  qs_cache_load (NULL);
  qs_cache_schedule_save (NULL);
  qs_cache_stop ();
}

/* Arquivo 0600, diretório 0700. */
static void
test_perms (void)
{
  QuickSettings *qs = new_qs ();
  gchar         *path = ini_path ();
  gchar         *dir = g_path_get_dirname (path);
  GStatBuf       st;

  rm_ini ();
  qs_cache_save (qs);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpuint ((guint) (st.st_mode & 0777), ==, 0600);
  g_assert_cmpint (g_stat (dir, &st), ==, 0);
  g_assert_cmpuint ((guint) (st.st_mode & 0777), ==, 0700);
  g_free (dir);
  g_free (path);
  free_qs (qs);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-cache-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* Setar XDG ANTES de gtk_init_check: o GLib cacheia os dirs do usuário. */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/cache/save_wifi_bt", test_save_wifi_bt);
  g_test_add_func ("/cache/save_defaults", test_save_defaults);
  g_test_add_func ("/cache/load_widgets", test_load_widgets);
  g_test_add_func ("/cache/load_stale", test_load_stale);
  g_test_add_func ("/cache/load_dead", test_load_dead);
  g_test_add_func ("/cache/schedule_stop", test_schedule_stop);
  g_test_add_func ("/cache/schedule_fires", test_schedule_fires);
  g_test_add_func ("/cache/corrupt_ini", test_corrupt_ini);
  g_test_add_func ("/cache/null_guards", test_null_guards);
  g_test_add_func ("/cache/perms", test_perms);
  return g_test_run ();
}
