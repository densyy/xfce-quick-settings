/* Testes de lifecycle: timers, stops, teardown e UI mínima offscreen.
 *
 * Precisam de display (há :0 real); widgets nunca são mostrados.
 * Sem display os casos dão skip. O PATH é prefixado com tests/fakebin
 * para os fluxos async (upower).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sqlite3.h>

#include "qs-core.h"
#include "qs-cache.h"
#include "qs-sysmon.h"
#include "qs-power.h"
#include "qs-notify.h"
#include "qs-notify-store.h"
#include "qs-display.h"

static gboolean  have_display = FALSE;
static gchar    *tmp_root = NULL;

static QuickSettings *
new_qs (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs->refcount = 1;
  return qs;
}

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static gboolean
need_display (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return FALSE;
    }
  return TRUE;
}

/* sysmon: click no tile liga o timer; stop desliga e é idempotente. */
static void
test_sysmon_click_stop (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  GList         *kids;

  if (!need_display ())
    return;
  qs = new_qs ();
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->stack = gtk_stack_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = GTK_WIDGET (qs->stack);
  sysmon_build_section (box, stack, qs);
  g_assert_cmpuint (qs->sysmon_timer, ==, 0);

  kids = gtk_container_get_children (GTK_CONTAINER (box));
  g_assert_nonnull (kids);
  g_signal_emit_by_name (kids->data, "clicked");
  g_assert_cmpuint (qs->sysmon_timer, !=, 0);

  sysmon_stop (qs);
  g_assert_cmpuint (qs->sysmon_timer, ==, 0);
  sysmon_stop (qs);
  g_list_free (kids);
  qs_unref (qs);
}

/* power: fluxo upower completo (fakebin) — 42%, ícone good, timer ligado. */
static void
test_power_upower_flow (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GMainLoop     *loop;
  const gchar   *label;
  const gchar   *status;
  const gchar   *icon = NULL;
  GtkImageType   storage;

  if (!need_display ())
    return;
  qs = new_qs ();
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  power_build_section (box, qs);
  power_refresh (qs);

  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (2500, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  label = gtk_label_get_text (GTK_LABEL (qs->bat_label));
  g_assert_cmpstr (label, ==, "42%");
  status = gtk_label_get_text (GTK_LABEL (qs->bat_status));
  g_assert_nonnull (g_strstr_len (status, -1, "Time to empty"));
  storage = gtk_image_get_storage_type (GTK_IMAGE (qs->bat_icon));
  g_assert_cmpint (storage, ==, GTK_IMAGE_ICON_NAME);
  gtk_image_get_icon_name (GTK_IMAGE (qs->bat_icon), &icon, NULL);
  g_assert_cmpstr (icon, ==, "battery-good-symbolic");
  g_assert_cmpuint (qs->power_timer, !=, 0);

  power_stop (qs);
  g_assert_cmpuint (qs->power_timer, ==, 0);
  power_stop (qs);
  qs_unref (qs);
}

/* bright_stop e qs_cache_stop são idempotentes e NULL-safe. */
static void
test_stops (void)
{
  QuickSettings *qs = new_qs ();

  bright_stop ();
  bright_stop ();
  qs_cache_stop ();
  qs_cache_stop ();
  qs_cache_stop ();
  qs_unref (qs);
}

/* Cria um banco de notificações com nrows linhas (timestamps recentes). */
static void
make_db (gint nrows)
{
  gchar   *path = g_build_filename (tmp_root, "xfce4", "notifyd",
                                    "log.sqlite", NULL);
  gchar   *dir = g_path_get_dirname (path);
  sqlite3 *db = NULL;
  gint64   base = g_get_real_time () - (gint64) nrows * 1000000;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_remove (path);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
                                 "CREATE TABLE notifications("
                                 "app_name TEXT, summary TEXT, body TEXT, "
                                 "timestamp INTEGER);",
                                 NULL, NULL, NULL),
                   ==, SQLITE_OK);
  for (gint i = 0; i < nrows; i++)
    {
      gchar *sql = g_strdup_printf ("INSERT INTO notifications VALUES("
                                    "'App', 'Sum', 'Body', %"
                                    G_GINT64_FORMAT ");",
                                    base + (gint64) i);

      g_assert_cmpint (sqlite3_exec (db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
      g_free (sql);
    }
  g_assert_cmpint (sqlite3_close (db), ==, SQLITE_OK);
  g_free (dir);
  g_free (path);
}

/* notify async: worker lê o DB e o tile mostra a contagem. */
static void
test_notify_async_tile (void)
{
  QuickSettings *qs;
  GMainLoop     *loop;
  const gchar   *text;

  if (!need_display ())
    return;
  qs = new_qs ();
  make_db (3);
  qs->notify_sub = gtk_label_new ("…");
  notify_refresh_tile_async (qs);

  loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (2500, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  text = gtk_label_get_text (GTK_LABEL (qs->notify_sub));
  g_assert_cmpstr (text, ==, "3");
  qs_cache_stop ();
  qs_unref (qs);
}

/* notify_update_tile: no-op seguro no primeiro run. */
static void
test_notify_update_no_crash (void)
{
  QuickSettings *qs;

  if (!need_display ())
    return;
  qs = new_qs ();
  qs->notify_sub = gtk_label_new ("…");
  notify_update_tile (qs);
  notify_refresh_tile_async (NULL);
  qs_unref (qs);
}

/* qs_ref/qs_unref balanceado libera sem crash. */
static void
test_ref_unref (void)
{
  QuickSettings *qs = new_qs ();

  g_free (qs->wifi_ssid);
  qs->wifi_ssid = g_strdup ("Casa");
  qs_ref (qs);
  qs_unref (qs);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gchar *fakebin;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-lifecycle-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* Setar XDG ANTES de gtk_init_check: o GLib cacheia os dirs do usuário. */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  /* upower falso na frente do PATH (prefix, não substitui). */
  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  {
    gchar *prefixed;
    const gchar *old_path = g_getenv ("PATH");

    prefixed = g_strdup_printf ("%s:%s", fakebin,
                                old_path ? old_path : "/usr/bin:/bin");
    g_setenv ("PATH", prefixed, TRUE);
    g_free (prefixed);
  }
  g_free (fakebin);
  g_free (cwd);

  g_test_add_func ("/lifecycle/sysmon_click_stop", test_sysmon_click_stop);
  g_test_add_func ("/lifecycle/power_upower_flow", test_power_upower_flow);
  g_test_add_func ("/lifecycle/stops", test_stops);
  g_test_add_func ("/lifecycle/notify_async_tile", test_notify_async_tile);
  g_test_add_func ("/lifecycle/notify_update_no_crash", test_notify_update_no_crash);
  g_test_add_func ("/lifecycle/ref_unref", test_ref_unref);
  return g_test_run ();
}
