/* test-e2e.c — testes end-to-end por domínio (grupo E2E).
 *
 * Dirige a UI real (builders públicos) via sinais GTK com fakes no PATH e
 * valida os comandos externos gerados (argv.log) + efeitos na UI:
 * wifi toggle/connect, bt power/toggle-info, áudio vol/sink, brilho,
 * nightlight, sessão logout, display mode/scale, notify clear.
 * Power tile já coberto em test-lifecycle.c.
 *
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sqlite3.h>

#include "qs-core.h"
#include "qs-net.h"
#include "qs-audio.h"
#include "qs-display.h"
#include "qs-power.h"
#include "qs-notify.h"
#include "qs-notify-store.h"
#include "qs-session.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
static gchar   *old_path = NULL;

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

/* --- fake nmcli por teste (lista configurável + log de argv) --- */

static void
write_nmcli_fake (const gchar *dir)
{
  const gchar *script =
    "#!/bin/sh\n"
    "LOG=\"${QS_FAKE_LOG:-/dev/null}\"\n"
    "printf 'nmcli argc=%d cmd=%s\\n' \"$#\" \"$*\" >> \"$LOG\"\n"
    "case \"$*\" in\n"
    "  \"radio wifi\") printf '%s\\n' \"${QS_FAKE_WIFI_STATE:-enabled}\" ;;\n"
    "  *\"device wifi list\"*) printf '%s\\n' \"${QS_FAKE_WIFI_LINE:-}\" ;;\n"
    "  *\"connection show\"*) printf '' ;;\n"
    "esac\n"
    "exit 0\n";
  gchar *path = g_build_filename (dir, "nmcli", NULL);

  g_file_set_contents (path, script, -1, NULL);
  g_chmod (path, 0755);
  g_free (path);
}

static gchar *
read_log (const gchar *path)
{
  gchar *s = NULL;

  g_file_get_contents (path, &s, NULL, NULL);
  return (s != NULL) ? s : g_strdup ("");
}

static gboolean
log_contains (const gchar *path, const gchar *substr)
{
  gchar   *log = read_log (path);
  gboolean hit = (g_strstr_len (log, -1, substr) != NULL);

  g_free (log);
  return hit;
}

static gboolean
wait_for_log (const gchar *path, const gchar *substr, gint timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_contains (path, substr))
        return TRUE;
      run_loop_ms (100);
      waited += 100;
    }
  return log_contains (path, substr);
}

static GtkWidget *
find_button_by_action (GtkWidget *root, const gchar *action)
{
  if (GTK_IS_BUTTON (root))
    {
      const gchar *a = g_object_get_data (G_OBJECT (root), "qs-action");

      if (g_strcmp0 (a, action) == 0)
        return root;
    }
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL; l = l->next)
        {
          hit = find_button_by_action (GTK_WIDGET (l->data), action);
          if (hit != NULL)
            break;
        }
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

/* --- wifi toggle --- */

static void
test_e2e_wifi_toggle (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *stack;
  GtkWidget     *back = NULL;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-wifitog-XXXXXX", NULL);
  g_assert_nonnull (dir);
  write_nmcli_fake (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  stack = gtk_stack_new ();
  wifi_build_page (stack, qs, &back);
  qs->wifi_tile = gtk_button_new ();
  qs->wifi_sub = gtk_label_new ("");
  gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), TRUE);
  g_assert_true (wait_for_log (log, "radio wifi on", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- wifi connect (rede aberta) --- */

static void
test_e2e_wifi_connect (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *stack;
  GtkWidget     *back = NULL;
  GList         *kids;
  GtkWidget     *row = NULL;
  GList         *l;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-wificonn-XXXXXX", NULL);
  g_assert_nonnull (dir);
  write_nmcli_fake (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_setenv ("QS_FAKE_WIFI_LINE", " :OpenNet:80:--", TRUE);
  /* Aponta o nmcli para o fake deste teste (como no wifi-security). */
  {
    const gchar *old_path = g_getenv ("PATH");
    gchar       *new_path = g_strdup_printf ("%s:%s", dir,
                                             old_path != NULL ? old_path : "/usr/bin:/bin");

    g_setenv ("PATH", new_path, TRUE);
    g_free (new_path);
  }

  stack = gtk_stack_new ();
  wifi_build_page (stack, qs, &back);
  qs->stack = stack;
  qs->wifi_tile = gtk_button_new ();
  qs->wifi_sub = gtk_label_new ("");
  g_assert_nonnull (qs->wifi_status);
  wifi_show_page (qs);
  /* Espera a row da rede aberta aparecer. */
  {
    gint waited = 0;

    while (waited < 10000 && row == NULL)
      {
        kids = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
        for (l = kids; l != NULL; l = l->next)
          {
            const gchar *ssid = g_object_get_data (G_OBJECT (l->data), "qs-ssid");

            if (g_strcmp0 (ssid, "OpenNet") == 0)
              {
                row = GTK_WIDGET (l->data);
                break;
              }
          }
        g_list_free (kids);
        if (row == NULL)
          {
            run_loop_ms (100);
            waited += 100;
          }
      }
  }
  g_assert_nonnull (row);
  g_signal_emit_by_name (qs->wifi_list, "row-activated", row);
  g_assert_true (wait_for_log (log, "device wifi connect", 10000));
  /* Quoting: SSID vai como argv único (argc>=4, sem split de shell). */
  g_assert_true (log_contains (log, "argc=4")
                 || log_contains (log, "argc=5")
                 || log_contains (log, "argc=6"));
  run_loop_ms (1000);

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- bt power --- */

static void
test_e2e_bt_power (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *stack;
  GtkWidget     *back = NULL;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-btpow-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  stack = gtk_stack_new ();
  bt_build_page (stack, qs, &back);
  qs->bt_tile = gtk_button_new ();
  qs->bt_sub = gtk_label_new ("");
  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  g_assert_true (wait_for_log (log, "power on", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- bt toggle info --- */

static void
test_e2e_bt_toggle_info (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *stack;
  GtkWidget     *back = NULL;
  GtkWidget     *row;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-btinfo-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  stack = gtk_stack_new ();
  bt_build_page (stack, qs, &back);
  qs->bt_tile = gtk_button_new ();
  qs->bt_sub = gtk_label_new ("");
  g_assert_nonnull (qs->bt_status);
  row = gtk_list_box_row_new ();
  gtk_container_add (GTK_CONTAINER (qs->bt_list), row);
  gtk_widget_show_all (row);
  g_object_set_data (G_OBJECT (row), "qs-mac", (gpointer) "AA:BB:CC:DD:EE:FF");
  g_object_set_data (G_OBJECT (row), "qs-name", (gpointer) "TestDevice");
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_for_log (log, "info AA:BB:CC:DD:EE:FF", 10000));
  /* Deixa a cadeia async (info → próximo passo) assentar antes do free. */
  run_loop_ms (1500);

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- áudio: slider de volume --- */

static void
test_e2e_audio_volume (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-audiovol-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);
  gtk_range_set_value (GTK_RANGE (qs->vol_scale), 55.0);
  g_assert_true (wait_for_log (log, "set-sink-volume @DEFAULT_SINK@ 55%", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- áudio: troca de sink --- */

static void
test_e2e_audio_sink (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-audiosink-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);
  /* Backend field mirrors post-refresh state (probe itself is covered in
   * test-audio.c); without it set_default_sink takes neither branch. */
  qs->audio_backend = AUDIO_PACTL;
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->sink_combo), "test-sink", "Test Sink");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->sink_combo), "test-sink");
  g_assert_true (wait_for_log (log, "set-default-sink", 10000));
  g_assert_true (log_contains (log, "test-sink"));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- brilho: slider --- */

static void
test_e2e_brightness_slider (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-bright-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
  run_loop_ms (800);
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_for_log (log, "brightnessctl", 10000));
  g_assert_true (log_contains (log, "70"));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- nightlight: preset 6500K --- */

static void
test_e2e_nightlight_preset (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *night_page;
  GList         *kids;
  GList         *l;
  GtkWidget     *day = NULL;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-night-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
  run_loop_ms (800);
  night_page = gtk_stack_get_child_by_name (GTK_STACK (stack), "night");
  g_assert_nonnull (night_page);
  kids = gtk_container_get_children (GTK_CONTAINER (night_page));
  for (l = kids; l != NULL; l = l->next)
    {
      GtkWidget *w = GTK_WIDGET (l->data);

      if (GTK_IS_CONTAINER (w))
        {
          GList *sub = gtk_container_get_children (GTK_CONTAINER (w));
          GList *s;

          for (s = sub; s != NULL; s = s->next)
            {
              const gchar *t = g_object_get_data (G_OBJECT (s->data), "qs-temp");

              if (GTK_IS_TOGGLE_BUTTON (s->data) && g_strcmp0 (t, "6500K") == 0)
                day = GTK_WIDGET (s->data);
            }
          g_list_free (sub);
        }
      else if (GTK_IS_TOGGLE_BUTTON (w))
        {
          const gchar *t = g_object_get_data (G_OBJECT (w), "qs-temp");

          if (g_strcmp0 (t, "6500K") == 0)
            day = w;
        }
      if (day != NULL)
        break;
    }
  g_list_free (kids);
  g_assert_nonnull (day);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (day), TRUE);
  g_assert_true (wait_for_log (log, "redshift -P -O 6500K", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- sessão: logout --- */

static void
test_e2e_session_logout (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *btn;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-session-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  qs->stack = stack;
  session_build_section (box, stack, qs);
  btn = find_button_by_action (stack, "logout");
  g_assert_nonnull (btn);
  g_signal_emit_by_name (btn, "clicked");
  g_assert_true (wait_for_log (log, "--logout", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

static gboolean
wait_for_combo_id (GtkComboBox *combo, const gchar *id, gint timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      const gchar *active = gtk_combo_box_get_active_id (combo);
      gboolean hit = (g_strcmp0 (active, id) == 0);

      if (hit)
        return TRUE;
      run_loop_ms (100);
      waited += 100;
    }
  {
    const gchar *active = gtk_combo_box_get_active_id (combo);
    gboolean hit = (g_strcmp0 (active, id) == 0);

    return hit;
  }
}

/* --- display: troca de modo --- */

static void
test_e2e_display_mode (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-dispmode-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
  /* espera o probe preencher (mon = primeiro = HDMI-1) */
    display_refresh_tile (qs); /* dispara o probe async (xrandr --query) */
g_assert_true (wait_for_combo_id (GTK_COMBO_BOX (qs->mon_combo), "HDMI-1", 10000));
  gtk_widget_set_sensitive (qs->mon_combo, TRUE);
  gtk_widget_set_sensitive (qs->mode_combo, TRUE);
  /* troca de monitor (usa o dump em cache, sem wipe) e depois de modo */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->mon_combo), "DP-1"));
  run_loop_ms (500);
  gtk_widget_set_sensitive (qs->mode_combo, TRUE);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->mode_combo), "1024x768"));
  g_assert_true (wait_for_log (log, "xrandr --output DP-1 --mode 1024x768", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- display: troca de escala --- */

static void
test_e2e_display_scale (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-dispscale-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
    display_refresh_tile (qs); /* dispara o probe async (xrandr --query) */
g_assert_true (wait_for_combo_id (GTK_COMBO_BOX (qs->mon_combo), "HDMI-1", 10000));
  gtk_widget_set_sensitive (qs->mon_combo, TRUE);
  gtk_widget_set_sensitive (qs->scale_combo, TRUE);
  if (!gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->scale_combo), "1.5"))
    {
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->scale_combo), "1.5", "1.5");
      g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->scale_combo), "1.5"));
    }
  g_assert_true (wait_for_log (log, "--scale 1.5x1.5", 10000));

  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

/* --- notify: clear --- */

static void
test_e2e_notify_clear (void)
{
  QuickSettings *qs = qs_new ();
  gchar         *dir;
  gchar         *log;
  gchar         *dbp;
  gchar         *dbdir;
  sqlite3       *db = NULL;
  GtkWidget     *box;
  GtkWidget     *stack;
  gint           i;
  gint           waited = 0;
  guint          n = 0;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  dir = g_dir_make_tmp ("qs-test-e2e-notify-XXXXXX", NULL);
  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  dbp = notify_db_path ();
  dbdir = g_path_get_dirname (dbp);
  g_mkdir_with_parents (dbdir, 0700);
  g_remove (dbp);
  g_assert_cmpint (sqlite3_open (dbp, &db), ==, SQLITE_OK);
  sqlite3_exec (db,
                "CREATE TABLE notifications"
                "(app_name TEXT, summary TEXT, body TEXT, timestamp INTEGER);",
                NULL, NULL, NULL);
  for (i = 0; i < 3; i++)
    {
      gchar *sql = g_strdup_printf ("INSERT INTO notifications VALUES"
                                    "('App%d', 'Sum%d', 'Body%d', %" G_GINT64_FORMAT ");",
                                    i, i, i,
                                    g_get_real_time () - (3 - i) * 1000000);

      sqlite3_exec (db, sql, NULL, NULL, NULL);
      g_free (sql);
    }
  sqlite3_close (db);

  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  qs->stack = stack;
  notify_build_section (box, stack, qs);
  gtk_widget_show_all (stack); /* visibilidade: get_visible_child_name exige filho visivel */
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "notices");
  /* Espera as rows aparecerem. */
  while (waited < 10000 && n == 0)
    {
      GList *kids = gtk_container_get_children (GTK_CONTAINER (qs->notify_list));

      n = g_list_length (kids);
      g_list_free (kids);
      if (n == 0)
        {
          run_loop_ms (100);
          waited += 100;
        }
    }
  g_assert_cmpuint (n, >, 0);
  g_signal_emit_by_name (qs->notify_clear, "clicked");
  run_loop_ms (500);
  {
    GList *notices = list_notices ();

    g_assert_true (notices == NULL);
    g_list_free_full (notices, notice_free);
  }

  g_free (dbdir);
  g_free (dbp);
  g_free (log);
  g_free (dir);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *cwd;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-e2e-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* XDG antes do gtk_init (GLib cacheia os dirs no primeiro uso). */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  /* PATH com os fakes do repo na frente (prefix, nunca replace). */
  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  old_path = g_strdup (g_getenv ("PATH"));
  if (old_path != NULL)
    {
      gchar *prefixed = g_strjoin (":", fakebin, old_path, NULL);

      g_setenv ("PATH", prefixed, TRUE);
      g_free (prefixed);
    }
  else
    {
      g_setenv ("PATH", fakebin, TRUE);
    }
  g_free (cwd);

  g_test_add_func ("/e2e/wifi_toggle", test_e2e_wifi_toggle);
  g_test_add_func ("/e2e/wifi_connect", test_e2e_wifi_connect);
  g_test_add_func ("/e2e/bt_power", test_e2e_bt_power);
  g_test_add_func ("/e2e/bt_toggle_info", test_e2e_bt_toggle_info);
  g_test_add_func ("/e2e/audio_volume", test_e2e_audio_volume);
  g_test_add_func ("/e2e/audio_sink", test_e2e_audio_sink);
  g_test_add_func ("/e2e/brightness_slider", test_e2e_brightness_slider);
  g_test_add_func ("/e2e/nightlight_preset", test_e2e_nightlight_preset);
  g_test_add_func ("/e2e/session_logout", test_e2e_session_logout);
  g_test_add_func ("/e2e/display_mode", test_e2e_display_mode);
  g_test_add_func ("/e2e/display_scale", test_e2e_display_scale);
  g_test_add_func ("/e2e/notify_clear", test_e2e_notify_clear);

  return g_test_run ();
}
