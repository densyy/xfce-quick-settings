/* test-power-flow.c — fluxo da página Battery (qs-power.c).
 *
 * Cobre upower fake (sucesso, sem bateria, falha de -e e de -i), fallback
 * sysfs (diretório real /sys/class/power_supply, só leitura, e raízes
 * temporárias via QS_POWER_SUPPLY_DIR), estados charging/fully-charged/empty,
 * power_tick (dead/continue), guarda de loading e hide do popup parando o
 * timer. NUNCA chama upower real: o PATH aponta para tests/fakebin.
 *
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-power.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
static gchar   *fake_path = NULL;
static gchar   *emptybin = NULL;
static gchar   *old_path = NULL;

static const gchar *DETAIL_CHARGING =
  "  state: charging\n"
  "  percentage: 20%\n"
  "  time to full: 40 minutes\n";

static const gchar *DETAIL_FULL =
  "  state: fully-charged\n"
  "  percentage: 100%\n"
  "  time to empty: 1.0 hours\n";

static const gchar *DETAIL_EMPTY =
  "  state: empty\n"
  "  percentage: 0%\n"
  "  time to empty: 5 minutes\n";

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *box;
} PowerFixture;

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

/* PATH normal do teste: fakes do repo na frente (upower falso). */
static void
path_use_fake (void)
{
  g_setenv ("PATH", fake_path, TRUE);
}

/* PATH sem upower (dir temporário vazio): força o fallback sysfs. */
static void
path_use_empty (void)
{
  g_setenv ("PATH", emptybin, TRUE);
}

static void
path_restore (void)
{
  if (old_path != NULL)
    g_setenv ("PATH", old_path, TRUE);
}

static void
clear_power_env (void)
{
  g_unsetenv ("QS_FAKE_UPOWER_NO_BATTERY");
  g_unsetenv ("QS_FAKE_UPOWER_LIST_FAIL");
  g_unsetenv ("QS_FAKE_UPOWER_DETAIL_FAIL");
  g_unsetenv ("QS_FAKE_UPOWER_DETAIL");
  g_unsetenv ("QS_POWER_SUPPLY_DIR");
}

static void
fixture_init (PowerFixture *f)
{
  f->qs = qs_new ();
  f->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  f->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->qs->popup = f->popup;
  power_build_section (f->box, f->qs);
}

/* Drena async pendente antes de destruir (callback não pode tocar em
 * widget morto) e cancela o timer. */
static void
fixture_clear (PowerFixture *f)
{
  power_stop (f->qs);
  run_loop_ms (800);
  power_stop (f->qs);
  gtk_widget_destroy (f->box);
  gtk_widget_destroy (f->popup);
  f->qs->popup = NULL;
  qs_unref (f->qs);
}

static void
assert_icon (GtkWidget *image, const gchar *expected)
{
  const gchar *name = NULL;

  g_assert_cmpint (gtk_image_get_storage_type (GTK_IMAGE (image)), ==,
                   GTK_IMAGE_ICON_NAME);
  gtk_image_get_icon_name (GTK_IMAGE (image), &name, NULL);
  g_assert_cmpstr (name, ==, expected);
}

static void
assert_power_none (PowerFixture *f)
{
  g_assert_false (gtk_widget_get_sensitive (f->qs->bat_box));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->bat_label)), ==,
                   "—");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->bat_status)), ==,
                   "No battery detected");
  g_assert_cmpuint (f->qs->power_timer, ==, 0);
}

/* Confere "NN%" (sem sinal, dentro de 0..100). */
static void
assert_percent_label (GtkWidget *label, gint expected)
{
  const gchar *text = gtk_label_get_text (GTK_LABEL (label));
  gchar       *end = NULL;
  gint64       value = g_ascii_strtoll (text, &end, 10);

  g_assert_nonnull (end);
  g_assert_cmpstr (end, ==, "%");
  g_assert_cmpint ((gint) value, ==, expected);
}

static gboolean
real_supply_has_battery (void)
{
  GDir        *dir = g_dir_open ("/sys/class/power_supply", 0, NULL);
  const gchar *entry;
  gboolean     found = FALSE;

  if (dir == NULL)
    return FALSE;
  while (!found && (entry = g_dir_read_name (dir)) != NULL)
    {
      gchar *type_path = g_build_filename ("/sys/class/power_supply", entry,
                                           "type", NULL);
      gchar *type = NULL;

      if (g_file_get_contents (type_path, &type, NULL, NULL))
        {
          if (g_strcmp0 (g_strstrip (type), "Battery") == 0)
            found = TRUE;
          g_free (type);
        }
      g_free (type_path);
    }
  g_dir_close (dir);
  return found;
}

/* --- casos upower (fakebin) --- */

static void
test_flow_discharging_timer_hide (void)
{
  PowerFixture f;
  guint         timer;
  const gchar  *status;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();
  fixture_init (&f);

  power_start (f.qs);
  run_loop_ms (1200);

  assert_percent_label (f.qs->bat_label, 42);
  status = gtk_label_get_text (GTK_LABEL (f.qs->bat_status));
  g_assert_nonnull (strstr (status, "Discharging"));
  g_assert_nonnull (strstr (status, "Time to empty"));
  g_assert_nonnull (strstr (status, "1.5 hours"));
  assert_icon (f.qs->bat_icon, "battery-good-symbolic");
  g_assert_true (gtk_widget_get_sensitive (f.qs->bat_box));
  timer = f.qs->power_timer;
  g_assert_cmpuint (timer, !=, 0);

  /* power_start repetido não duplica o timer (loading reabre apos o done). */
  power_start (f.qs);
  run_loop_ms (1200);
  g_assert_cmpuint (f.qs->power_timer, ==, timer);
  assert_percent_label (f.qs->bat_label, 42);

  /* hide do popup para o timer; power_stop é idempotente. */
  g_signal_emit_by_name (f.popup, "hide");
  g_assert_cmpuint (f.qs->power_timer, ==, 0);
  power_stop (f.qs);
  g_assert_cmpuint (f.qs->power_timer, ==, 0);

  fixture_clear (&f);
}

static void
test_flow_detail_states (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();

  g_setenv ("QS_FAKE_UPOWER_DETAIL", DETAIL_CHARGING, TRUE);
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_percent_label (f.qs->bat_label, 20);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_status)), ==,
                   "Charging · Time to full: 40 minutes");
  assert_icon (f.qs->bat_icon, "battery-caution-charging-symbolic");
  g_assert_cmpuint (f.qs->power_timer, !=, 0);
  fixture_clear (&f);

  g_setenv ("QS_FAKE_UPOWER_DETAIL", DETAIL_FULL, TRUE);
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_percent_label (f.qs->bat_label, 100);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_status)), ==,
                   "Fully charged · Time to empty: 1.0 hours");
  assert_icon (f.qs->bat_icon, "battery-full-charged-symbolic");
  fixture_clear (&f);

  g_setenv ("QS_FAKE_UPOWER_DETAIL", DETAIL_EMPTY, TRUE);
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_percent_label (f.qs->bat_label, 0);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_status)), ==,
                   "Empty · Time to empty: 5 minutes");
  assert_icon (f.qs->bat_icon, "battery-empty-symbolic");
  fixture_clear (&f);
}

static void
test_flow_no_battery_and_list_fail (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();

  g_setenv ("QS_FAKE_UPOWER_NO_BATTERY", "1", TRUE);
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_power_none (&f);
  assert_icon (f.qs->bat_icon, "battery-missing-symbolic");
  fixture_clear (&f);

  g_unsetenv ("QS_FAKE_UPOWER_NO_BATTERY");
  g_setenv ("QS_FAKE_UPOWER_LIST_FAIL", "1", TRUE);
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_power_none (&f);
  fixture_clear (&f);
}

static void
test_flow_detail_fail (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();
  g_setenv ("QS_FAKE_UPOWER_DETAIL_FAIL", "1", TRUE);
  fixture_init (&f);

  power_start (f.qs);
  run_loop_ms (1200);

  assert_power_none (&f);
  fixture_clear (&f);
}

/* upower existe mas o exec falha (interprete inexistente): -e retorna
 * ok=FALSE no run_command_async, sem tocar em binário real. */
static void
test_flow_spawn_fail (void)
{
  PowerFixture f;
  gchar        *dir = g_build_filename (tmp_root, "badbin", NULL);
  gchar        *script = g_build_filename (dir, "upower", NULL);
  gchar        *saved;

  if (skip_no_display ())
    return;
  clear_power_env ();
  g_mkdir_with_parents (dir, 0755);
  g_assert_true (g_file_set_contents (script, "#!/nonexistent/qs-interp\n",
                                      -1, NULL));
  g_assert_cmpint (g_chmod (script, 0755), ==, 0);
  saved = g_strdup (g_getenv ("PATH"));
  g_setenv ("PATH", dir, TRUE);

  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (1200);
  assert_power_none (&f);
  fixture_clear (&f);

  if (saved != NULL)
    g_setenv ("PATH", saved, TRUE);
  g_free (saved);
  g_free (script);
  g_free (dir);
}

/* Guarda de loading: refresh com a flag ligada não spawna de novo. */
static void
test_flow_loading_guard (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();
  fixture_init (&f);

  g_object_set_data (G_OBJECT (f.popup), "qs-power-loading",
                     GUINT_TO_POINTER (1));
  power_refresh (f.qs);
  run_loop_ms (400);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_label)), ==,
                   "—");
  g_object_set_data (G_OBJECT (f.popup), "qs-power-loading", NULL);

  /* popup NULL: refresh segue (sem flag) e o done atualiza os widgets. */
  f.qs->popup = NULL;
  power_refresh (f.qs);
  run_loop_ms (1200);
  assert_percent_label (f.qs->bat_label, 42);
  f.qs->popup = f.popup;

  fixture_clear (&f);
}

/* Callback async com qs já morto: solta a ref sem tocar em widget. */
static void
test_flow_async_dead (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_fake ();
  fixture_init (&f);

  power_refresh (f.qs);
  f.qs->dead = TRUE;
  run_loop_ms (800);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_label)), ==,
                   "—");

  f.qs->dead = FALSE;
  fixture_clear (&f);
}

/* --- fallback sysfs --- */

/* Raiz real /sys/class/power_supply: só leitura. O resultado depende da
 * máquina (com/sem bateria) e as duas formas são válidas. */
static void
test_flow_sysfs_real (void)
{
  PowerFixture f;

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_empty ();
  g_assert_null (g_find_program_in_path ("upower"));
  fixture_init (&f);

  power_start (f.qs);
  run_loop_ms (200);

  if (real_supply_has_battery ())
    {
      const gchar *text = gtk_label_get_text (GTK_LABEL (f.qs->bat_label));
      gchar       *end = NULL;
      gint64       value = g_ascii_strtoll (text, &end, 10);

      g_assert_cmpstr (end, ==, "%");
      g_assert_cmpint ((gint) value, >=, 0);
      g_assert_cmpint ((gint) value, <=, 100);
      g_assert_true (gtk_widget_get_sensitive (f.qs->bat_box));
      g_assert_cmpuint (f.qs->power_timer, !=, 0);
    }
  else
    {
      assert_power_none (&f);
    }
  fixture_clear (&f);
  path_restore ();
}

static void
write_supply_file (const gchar *root,
                   const gchar *entry,
                   const gchar *file,
                   const gchar *value)
{
  gchar *dir = g_build_filename (root, entry, NULL);
  gchar *path = g_build_filename (root, entry, file, NULL);

  g_mkdir_with_parents (dir, 0755);
  g_assert_true (g_file_set_contents (path, value, -1, NULL));
  g_free (dir);
  g_free (path);
}

static gchar *
make_fake_supply_root (const gchar *name)
{
  gchar *root = g_build_filename (tmp_root, name, NULL);

  g_mkdir_with_parents (root, 0755);
  return root;
}

static void
test_flow_sysfs_fake_battery (void)
{
  PowerFixture f;
  gchar        *root = make_fake_supply_root ("ps-battery");

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_empty ();
  write_supply_file (root, "ADP0", "type", "Mains\n");
  write_supply_file (root, "BAT0", "type", "Battery\n");
  write_supply_file (root, "BAT0", "capacity", "77\n");
  write_supply_file (root, "BAT0", "status", "Charging\n");
  g_setenv ("QS_POWER_SUPPLY_DIR", root, TRUE);

  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (200);

  assert_percent_label (f.qs->bat_label, 77);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_status)), ==,
                   "Charging");
  assert_icon (f.qs->bat_icon, "battery-good-charging-symbolic");
  g_assert_cmpuint (f.qs->power_timer, !=, 0);
  fixture_clear (&f);
  path_restore ();
  g_free (root);
}

static void
test_flow_sysfs_fake_no_battery (void)
{
  PowerFixture f;
  gchar        *root = make_fake_supply_root ("ps-nobat");

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_empty ();
  write_supply_file (root, "ADP0", "type", "Mains\n");
  write_supply_file (root, "misc", "capacity", "10\n");
  g_setenv ("QS_POWER_SUPPLY_DIR", root, TRUE);

  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (200);
  assert_power_none (&f);
  fixture_clear (&f);

  /* capacity inválida: a bateria existe mas o parse falha => sem bateria. */
  write_supply_file (root, "BAT0", "type", "Battery\n");
  write_supply_file (root, "BAT0", "capacity", "junk\n");
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (200);
  assert_power_none (&f);
  fixture_clear (&f);

  path_restore ();
  g_free (root);
}

static void
test_flow_sysfs_fake_status_missing (void)
{
  PowerFixture f;
  gchar        *root = make_fake_supply_root ("ps-nostatus");

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_empty ();
  write_supply_file (root, "BAT0", "type", "Battery\n");
  write_supply_file (root, "BAT0", "capacity", "50\n");
  g_setenv ("QS_POWER_SUPPLY_DIR", root, TRUE);

  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (200);
  assert_percent_label (f.qs->bat_label, 50);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->bat_status)), ==,
                   "Discharging");
  assert_icon (f.qs->bat_icon, "battery-good-symbolic");
  fixture_clear (&f);

  /* diretório inexistente: g_dir_open falha => sem bateria. */
  {
    gchar *missing = g_build_filename (root, "missing", NULL);

    g_setenv ("QS_POWER_SUPPLY_DIR", missing, TRUE);
    g_free (missing);
  }
  fixture_init (&f);
  power_start (f.qs);
  run_loop_ms (200);
  assert_power_none (&f);
  fixture_clear (&f);

  path_restore ();
  g_free (root);
}

/* --- power_tick --- */

static void
test_flow_tick_dead (void)
{
  QuickSettings *qs = qs_new ();

  qs->dead = TRUE;
  qs->power_timer = 4242;
  g_assert_false (power_tick (qs));
  g_assert_cmpuint (qs->power_timer, ==, 0);
  qs_unref (qs);
}

static void
test_flow_tick_continue (void)
{
  PowerFixture f;
  gchar        *root = make_fake_supply_root ("ps-tick");

  if (skip_no_display ())
    return;
  clear_power_env ();
  path_use_empty ();
  write_supply_file (root, "BAT0", "type", "Battery\n");
  write_supply_file (root, "BAT0", "capacity", "88\n");
  write_supply_file (root, "BAT0", "status", "Discharging\n");
  g_setenv ("QS_POWER_SUPPLY_DIR", root, TRUE);
  fixture_init (&f);

  /* sysfs síncrono: o tick lê na hora e rearma. */
  g_assert_true (power_tick (f.qs));
  assert_percent_label (f.qs->bat_label, 88);
  fixture_clear (&f);

  /* upower async: o tick agenda o refresh e segue. */
  path_use_fake ();
  clear_power_env ();
  fixture_init (&f);
  g_assert_true (power_tick (f.qs));
  run_loop_ms (1200);
  assert_percent_label (f.qs->bat_label, 42);
  fixture_clear (&f);

  path_restore ();
  g_free (root);
}

int
main (int argc, char **argv)
{
  gchar *cwd;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-power-flow-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* XDG antes do gtk_init (GLib cacheia os dirs no primeiro uso). */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  old_path = g_strdup (g_getenv ("PATH"));
  fake_path = g_strdup_printf ("%s:%s", fakebin,
                               old_path != NULL ? old_path : "/usr/bin:/bin");
  emptybin = g_build_filename (tmp_root, "emptybin", NULL);
  g_mkdir_with_parents (emptybin, 0755);
  g_free (cwd);

  g_test_add_func ("/power-flow/discharging_timer_hide",
                   test_flow_discharging_timer_hide);
  g_test_add_func ("/power-flow/detail_states", test_flow_detail_states);
  g_test_add_func ("/power-flow/no_battery_and_list_fail",
                   test_flow_no_battery_and_list_fail);
  g_test_add_func ("/power-flow/detail_fail", test_flow_detail_fail);
  g_test_add_func ("/power-flow/spawn_fail", test_flow_spawn_fail);
  g_test_add_func ("/power-flow/loading_guard", test_flow_loading_guard);
  g_test_add_func ("/power-flow/async_dead", test_flow_async_dead);
  g_test_add_func ("/power-flow/sysfs_real", test_flow_sysfs_real);
  g_test_add_func ("/power-flow/sysfs_fake_battery",
                   test_flow_sysfs_fake_battery);
  g_test_add_func ("/power-flow/sysfs_fake_no_battery",
                   test_flow_sysfs_fake_no_battery);
  g_test_add_func ("/power-flow/sysfs_fake_status_missing",
                   test_flow_sysfs_fake_status_missing);
  g_test_add_func ("/power-flow/tick_dead", test_flow_tick_dead);
  g_test_add_func ("/power-flow/tick_continue", test_flow_tick_continue);

  return g_test_run ();
}
