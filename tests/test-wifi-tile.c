/* test-wifi-tile.c — pintura e probe assíncrono do tile de Wi-Fi.
 *
 * Usa tests/fakebin/nmcli via PATH (nunca o nmcli real) e XDG temporário.
 * Cobre wifi_update_tile e wifi_refresh_tile_async: estado on/off, SSID,
 * indisponível, seq obsoleta, scanning e dead. Pula sem display.
 */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-net.h"
#include "qs-cache.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
static gchar   *old_path = NULL;
static gchar   *empty_dir = NULL;
static gchar   *log_path = NULL;

static QuickSettings *
qs_new (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs_ref (qs);
  return qs;
}

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static void
run_loop_ms (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static gboolean
skip_no_display (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return TRUE;
    }
  return FALSE;
}

static void
use_fake_path (void)
{
  gchar *path = g_strconcat (fakebin, ":", old_path, NULL);

  g_setenv ("PATH", path, TRUE);
  g_free (path);
}

static void
reset_env (void)
{
  use_fake_path ();
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_unsetenv ("QS_FAKE_NMCLI_SCAN_FAIL");
  g_unsetenv ("QS_FAKE_NMCLI_CONNECT_FAIL");
  g_remove (log_path);
}

static gchar *
read_log (void)
{
  gchar *text = NULL;

  if (!g_file_get_contents (log_path, &text, NULL, NULL))
    return g_strdup ("");
  return text;
}

static gboolean
log_contains (const gchar *needle)
{
  gchar   *text = read_log ();
  gboolean hit = strstr (text, needle) != NULL;

  g_free (text);
  return hit;
}

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *tile;
  GtkWidget     *sub;
} Tile;

static void
tile_init (Tile *t)
{
  t->qs = qs_new ();
  t->tile = gtk_button_new ();
  t->sub = gtk_label_new ("");
  t->qs->wifi_tile = t->tile;
  t->qs->wifi_sub = t->sub;
}

static void
tile_clear (Tile *t)
{
  qs_cache_stop ();
  gtk_widget_destroy (t->tile);
  gtk_widget_destroy (t->sub);
  qs_unref (t->qs);
}

static gboolean
tile_active (GtkWidget *tile)
{
  return gtk_style_context_has_class (gtk_widget_get_style_context (tile),
                                      "qs-tile-active");
}

typedef struct
{
  GtkWidget   *label;
  const gchar *text;
} LabelWait;

static gboolean
label_matches (gpointer data)
{
  LabelWait *w = data;

  return g_strcmp0 (gtk_label_get_text (GTK_LABEL (w->label)), w->text) == 0;
}

static gboolean
wait_label (GtkWidget   *label,
            const gchar *text)
{
  LabelWait w = { label, text };
  guint     waited = 0;

  while (waited < 10000)
    {
      if (label_matches (&w))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return label_matches (&w);
}

static void
test_update_tile (void)
{
  Tile *t;

  if (skip_no_display ())
    return;
  reset_env ();
  t = g_new0 (Tile, 1);
  tile_init (t);

  t->qs->wifi_on = FALSE;
  g_free (t->qs->wifi_ssid);
  t->qs->wifi_ssid = NULL;
  wifi_update_tile (t->qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (t->sub)), ==, "Off");
  g_assert_true (gtk_widget_get_sensitive (t->tile));
  g_assert_false (tile_active (t->tile));

  t->qs->wifi_on = TRUE;
  wifi_update_tile (t->qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (t->sub)), ==, "On");
  g_assert_true (tile_active (t->tile));

  t->qs->wifi_ssid = g_strdup ("");
  wifi_update_tile (t->qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (t->sub)), ==, "On");

  g_free (t->qs->wifi_ssid);
  t->qs->wifi_ssid = g_strdup ("Casa");
  wifi_update_tile (t->qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (t->sub)), ==, "Casa");

  g_setenv ("PATH", empty_dir, TRUE);
  wifi_update_tile (t->qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (t->sub)), ==, "Unavailable");
  g_assert_false (gtk_widget_get_sensitive (t->tile));
  g_assert_false (log_contains ("radio wifi"));

  use_fake_path ();
  tile_clear (t);
  g_free (t);
}

static void
test_probe_states (void)
{
  Tile *t;

  if (skip_no_display ())
    return;
  reset_env ();
  t = g_new0 (Tile, 1);
  tile_init (t);
  t->qs->wifi_ssid = g_strdup ("Casa");

  wifi_refresh_tile_async (t->qs);
  g_assert_true (wait_label (t->sub, "Casa"));
  g_assert_true (t->qs->wifi_on);
  g_assert_true (gtk_widget_get_sensitive (t->tile));
  g_assert_true (tile_active (t->tile));
  g_assert_true (log_contains ("radio wifi"));

  g_setenv ("QS_FAKE_WIFI_STATE", "disabled", TRUE);
  gtk_label_set_text (GTK_LABEL (t->sub), "pending");
  wifi_refresh_tile_async (t->qs);
  g_assert_true (wait_label (t->sub, "Off"));
  g_assert_false (t->qs->wifi_on);
  g_assert_false (tile_active (t->tile));

  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_free (t->qs->wifi_ssid);
  t->qs->wifi_ssid = NULL;
  gtk_label_set_text (GTK_LABEL (t->sub), "pending");
  wifi_refresh_tile_async (t->qs);
  g_assert_true (wait_label (t->sub, "On"));
  g_assert_true (t->qs->wifi_on);
  g_assert_true (tile_active (t->tile));

  g_remove (log_path);
  g_setenv ("PATH", empty_dir, TRUE);
  gtk_label_set_text (GTK_LABEL (t->sub), "pending");
  wifi_refresh_tile_async (t->qs);
  g_assert_true (wait_label (t->sub, "Unavailable"));
  g_assert_false (t->qs->wifi_on);
  g_assert_false (gtk_widget_get_sensitive (t->tile));

  use_fake_path ();
  tile_clear (t);
  g_free (t);
}

static void
test_probe_stale_scanning_dead (void)
{
  Tile *t;

  if (skip_no_display ())
    return;
  reset_env ();
  t = g_new0 (Tile, 1);
  tile_init (t);
  t->qs->wifi_ssid = g_strdup ("Casa");

  gtk_label_set_text (GTK_LABEL (t->sub), "pending");
  wifi_refresh_tile_async (t->qs);
  wifi_refresh_tile_async (t->qs);
  g_assert_true (wait_label (t->sub, "Casa"));
  run_loop_ms (100);
  g_assert_true (t->qs->wifi_on);

  g_remove (log_path);
  t->qs->scanning_wifi = TRUE;
  wifi_refresh_tile_async (t->qs);
  run_loop_ms (200);
  g_assert_false (log_contains ("radio wifi"));
  t->qs->scanning_wifi = FALSE;

  t->qs->dead = TRUE;
  wifi_refresh_tile_async (t->qs);
  run_loop_ms (300);
  g_assert_false (log_contains ("radio wifi"));

  tile_clear (t);
  g_free (t);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_log_set_always_fatal (G_LOG_FATAL_MASK);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-wifi-tile-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  fakebin = g_build_filename (g_get_current_dir (), "tests", "fakebin", NULL);
  empty_dir = g_build_filename (tmp_root, "empty", NULL);
  g_assert_cmpint (g_mkdir_with_parents (empty_dir, 0700), ==, 0);
  old_path = g_strdup (g_getenv ("PATH"));
  if (old_path == NULL)
    old_path = g_strdup ("/usr/bin:/bin");
  log_path = g_build_filename (tmp_root, "nmcli.log", NULL);
  g_setenv ("QS_FAKE_LOG", log_path, TRUE);

  g_test_add_func ("/wifi/tile/update", test_update_tile);
  g_test_add_func ("/wifi/tile/probe_states", test_probe_states);
  g_test_add_func ("/wifi/tile/probe_stale_scanning_dead",
                   test_probe_stale_scanning_dead);
  return g_test_run ();
}
