/* test-brightness-flow.c — fluxos de brilho por backend.
 *
 * Cada cenário roda em subprocesso (g_test_trap_subprocess) porque o backend
 * e o device ficam em cache estático em qs-brightness.c. PATH é sempre
 * restrito aos fakes; nenhum xrandr/brightnessctl real é executado.
 * Escrita sysfs só acontece em diretório temporário via QS_BACKLIGHT_DIR.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <string.h>

#include "qs-core.h"
#include "qs-display.h"

static gboolean have_display = FALSE;
static gchar   *repo_root = NULL;
static gchar   *fakebin_dir = NULL;
static gchar   *fake_ctl = NULL;
static gchar   *fake_xback = NULL;
static gchar   *fake_xrandr = NULL;

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
scenario_ready (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return FALSE;
    }
  if (!g_test_subprocess ())
    {
      g_test_skip ("runs isolated");
      return FALSE;
    }
  return TRUE;
}

static void
run_isolated (const gchar *path)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  g_test_trap_subprocess (path, 0, 0);
  g_test_trap_assert_passed ();
}

static void
run_isolated_data (gconstpointer data)
{
  run_isolated ((const gchar *) data);
}

/* gtk_widget_get_tooltip_text devolve string nova: liberar apos comparar. */
static void
assert_tooltip (GtkWidget   *widget,
                const gchar *expected)
{
  gchar *tip = gtk_widget_get_tooltip_text (widget);

  g_assert_cmpstr (tip, ==, expected);
  g_free (tip);
}

static gchar *
new_log (void)
{
  gchar *dir = g_dir_make_tmp ("qs-bright-log-XXXXXX", NULL);
  gchar *log;

  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_free (dir);
  return log;
}

static gboolean
log_has (const gchar *path,
         const gchar *needle)
{
  gchar   *text = NULL;
  gboolean hit;

  g_file_get_contents (path, &text, NULL, NULL);
  hit = (text != NULL && strstr (text, needle) != NULL);
  g_free (text);
  return hit;
}

static gboolean
wait_log (const gchar *path,
          const gchar *needle,
          guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_has (path, needle))
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return log_has (path, needle);
}

static void
set_path (const gchar *first,
          const gchar *second)
{
  gchar *path;

  if (second != NULL)
    path = g_strjoin (":", first, second, NULL);
  else
    path = g_strdup (first);
  g_setenv ("PATH", path, TRUE);
  g_free (path);
}

static gchar *
new_bin_dir (void)
{
  gchar *dir = g_dir_make_tmp ("qs-bright-bin-XXXXXX", NULL);

  g_assert_nonnull (dir);
  return dir;
}

static void
copy_tool (const gchar *dst_dir,
           const gchar *src)
{
  gchar *contents = NULL;
  gsize  len = 0;
  gchar *base = g_path_get_basename (src);
  gchar *dst = g_build_filename (dst_dir, base, NULL);

  g_assert_true (g_file_get_contents (src, &contents, &len, NULL));
  g_assert_true (g_file_set_contents (dst, contents, (gssize) len, NULL));
  g_assert_cmpint (g_chmod (dst, 0755), ==, 0);
  g_free (contents);
  g_free (dst);
  g_free (base);
}

static void
write_file (const gchar *dir,
            const gchar *name,
            const gchar *contents)
{
  gchar *path = g_build_filename (dir, name, NULL);

  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  g_free (path);
}

static gchar *
make_sysfs (const gchar *dev,
            const gchar *brightness,
            const gchar *max_brightness)
{
  gchar *base = g_dir_make_tmp ("qs-bright-sysfs-XXXXXX", NULL);
  gchar *dir;

  g_assert_nonnull (base);
  dir = g_build_filename (base, dev, NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  write_file (dir, "brightness", brightness);
  write_file (dir, "max_brightness", max_brightness);
  g_setenv ("QS_BACKLIGHT_DIR", base, TRUE);
  g_free (dir);
  return base;
}

static void
make_empty_sysfs (void)
{
  gchar *dir = g_dir_make_tmp ("qs-bright-empty-XXXXXX", NULL);

  g_assert_nonnull (dir);
  g_setenv ("QS_BACKLIGHT_DIR", dir, TRUE);
  g_free (dir);
}

static QuickSettings *
new_qs (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs_ref (qs);
  qs->bright_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                               BRIGHTNESS_MIN, 100.0, 1.0);
  gtk_scale_set_draw_value (GTK_SCALE (qs->bright_scale), FALSE);
  g_signal_connect (qs->bright_scale, "value-changed",
                    G_CALLBACK (on_brightness_changed), qs);
  return qs;
}

static void
free_qs (QuickSettings *qs)
{
  gtk_widget_destroy (qs->bright_scale);
  qs->bright_scale = NULL;
  qs_unref (qs);
}

static gboolean
wait_value (QuickSettings *qs,
            gdouble        expected,
            guint          timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (gtk_range_get_value (GTK_RANGE (qs->bright_scale)) == expected)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return gtk_range_get_value (GTK_RANGE (qs->bright_scale)) == expected;
}

static gboolean
wait_file (const gchar *path,
           const gchar *needle,
           guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      gchar   *text = NULL;
      gboolean hit;

      g_file_get_contents (path, &text, NULL, NULL);
      hit = (text != NULL && strstr (text, needle) != NULL);
      g_free (text);
      if (hit)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return FALSE;
}

/* --- CTL --- */

static void
test_ctl_refresh (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  sysfs = make_sysfs ("fakebl", "50", "100");
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));
  g_assert_true (gtk_widget_is_sensitive (qs->bright_scale));
  assert_tooltip (qs->bright_scale, "Screen brightness");
  g_assert_true (log_has (log, "brightnessctl -c backlight -m"));

  g_setenv ("QS_FAKE_BRIGHT_PCT", "2", TRUE);
  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 5.0, 2000));
  g_setenv ("QS_FAKE_BRIGHT_PCT", "150", TRUE);
  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 100.0, 2000));
  g_assert_false (log_has (log, " set "));

  free_qs (qs);
  g_free (sysfs);
  g_free (log);
}

static void
test_ctl_second_form (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  sysfs = make_sysfs ("fakebl", "50", "100");
  g_setenv ("QS_FAKE_BRIGHT_CTL_EMPTY", "1", TRUE);
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));
  g_assert_true (log_has (log, "brightnessctl -c backlight -m"));
  g_assert_true (log_has (log, "brightnessctl -m"));

  free_qs (qs);
  g_free (sysfs);
  g_free (log);
}

static void
test_ctl_fallback_sysfs (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  sysfs = make_sysfs ("fakebl", "60", "100");
  g_setenv ("QS_FAKE_BRIGHT_EMPTY", "1", TRUE);
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 60.0, 2000));
  g_assert_true (gtk_widget_is_sensitive (qs->bright_scale));
  g_assert_true (log_has (log, "brightnessctl -c backlight -m"));
  g_assert_true (log_has (log, "brightnessctl -m"));

  free_qs (qs);
  g_free (sysfs);
  g_free (log);
}

static void
test_ctl_fallback_xback (void)
{
  QuickSettings *qs;
  gchar         *log;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  g_setenv ("QS_FAKE_BRIGHT_EMPTY", "1", TRUE);
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 42.0, 2000));
  g_assert_true (log_has (log, "xbacklight -get"));

  free_qs (qs);
  g_free (log);
}

static void
test_ctl_fallback_xrandr (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  g_setenv ("QS_FAKE_BRIGHT_EMPTY", "1", TRUE);
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  copy_tool (bin, fake_xrandr);
  set_path (bin, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));
  g_assert_true (log_has (log, "xrandr --verbose"));

  free_qs (qs);
  g_free (bin);
  g_free (log);
}

static void
test_ctl_fallback_none (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  g_setenv ("QS_FAKE_BRIGHT_EMPTY", "1", TRUE);
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  run_loop_ms (300);
  g_assert_false (gtk_widget_is_sensitive (qs->bright_scale));
  assert_tooltip (qs->bright_scale, "Brightness unavailable");

  free_qs (qs);
  g_free (bin);
  g_free (log);
}

static void
test_ctl_fail_sysfs (void)
{
  QuickSettings *qs;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  sysfs = make_sysfs ("fakebl", "55", "100");
  g_setenv ("QS_FAKE_BRIGHT_FAIL", "1", TRUE);
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 55.0, 2000));

  free_qs (qs);
  g_free (sysfs);
}

static void
test_ctl_invalid_percent (void)
{
  QuickSettings *qs;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  sysfs = make_sysfs ("fakebl", "55", "100");
  g_setenv ("QS_FAKE_BRIGHT_PCT", "abc", TRUE);
  set_path (fakebin_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 55.0, 2000));

  free_qs (qs);
  g_free (sysfs);
}

static void
test_ctl_dead_callback (void)
{
  QuickSettings *qs;
  gchar         *bin;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  qs = new_qs ();

  display_refresh_brightness (qs);
  qs->dead = TRUE;
  run_loop_ms (300);
  g_assert_cmpfloat (gtk_range_get_value (GTK_RANGE (qs->bright_scale)), ==,
                     (gdouble) BRIGHTNESS_MIN);

  qs->dead = FALSE;
  free_qs (qs);
  g_free (sysfs);
  g_free (bin);
}

/* --- XBACK --- */

static void
test_xback_refresh_set (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *xback_dir;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  xback_dir = g_path_get_dirname (fake_xback);
  set_path (xback_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 42.0, 2000));
  g_assert_true (gtk_widget_is_sensitive (qs->bright_scale));

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_log (log, "xbacklight -set 70", 2000));
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 5.0);
  g_assert_true (wait_log (log, "xbacklight -set 5", 2000));
  g_assert_false (log_has (log, "xrandr"));

  free_qs (qs);
  g_free (xback_dir);
  g_free (log);
}

static void
test_xback_bad_to_xrandr (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  g_setenv ("QS_FAKE_XBACK_BAD", "1", TRUE);
  bin = new_bin_dir ();
  copy_tool (bin, fake_xback);
  copy_tool (bin, fake_xrandr);
  set_path (bin, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));
  g_assert_true (log_has (log, "xbacklight -get"));
  g_assert_true (log_has (log, "xrandr --verbose"));

  free_qs (qs);
  g_free (bin);
  g_free (log);
}

static void
test_xback_bad_disable (void)
{
  QuickSettings *qs;
  gchar         *xback_dir;

  if (!scenario_ready ())
    return;
  make_empty_sysfs ();
  g_setenv ("QS_FAKE_XBACK_BAD", "1", TRUE);
  xback_dir = g_path_get_dirname (fake_xback);
  set_path (xback_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  run_loop_ms (300);
  g_assert_false (gtk_widget_is_sensitive (qs->bright_scale));

  free_qs (qs);
  g_free (xback_dir);
}

/* --- XRANDR --- */

static void
test_xrandr_refresh_set (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *xrandr_dir;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  xrandr_dir = g_path_get_dirname (fake_xrandr);
  set_path (xrandr_dir, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));
  g_assert_true (log_has (log, "xrandr --verbose"));

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_log (log,
                           "xrandr --output HDMI-1 --brightness 0.70",
                           2000));
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 5.0);
  g_assert_true (wait_log (log,
                           "xrandr --output HDMI-1 --brightness 0.30",
                           2000));

  free_qs (qs);
  g_free (xrandr_dir);
  g_free (log);
}

static void
test_xrandr_set_miss (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *xrandr_dir;

  if (!scenario_ready ())
    return;
  log = new_log ();
  make_empty_sysfs ();
  xrandr_dir = g_path_get_dirname (fake_xrandr);
  set_path (xrandr_dir, NULL);
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 40.0);
  g_assert_true (wait_log (log,
                           "xrandr --output HDMI-1 --brightness 0.40",
                           2000));
  g_assert_true (log_has (log, "xrandr --verbose"));

  free_qs (qs);
  g_free (xrandr_dir);
  g_free (log);
}

/* --- setter CTL --- */

static void
test_ctl_set_dev (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_log (log, "brightnessctl -d fakebl set 70%", 2000));

  free_qs (qs);
  g_free (sysfs);
  g_free (bin);
  g_free (log);
}

static void
test_ctl_set_no_dev (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  make_empty_sysfs ();
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_log (log, "brightnessctl set 70%", 2000));
  g_assert_false (log_has (log, "-d "));

  free_qs (qs);
  g_free (bin);
  g_free (log);
}

/* --- SYSFS --- */

static void
test_sysfs_real_read (void)
{
  QuickSettings *qs;
  gchar         *bin;

  if (!scenario_ready ())
    return;
  g_unsetenv ("QS_BACKLIGHT_DIR");
  bin = new_bin_dir ();
  set_path (bin, NULL);
  qs = new_qs ();

  display_refresh_brightness (qs);
  run_loop_ms (200);
  if (!gtk_widget_is_sensitive (qs->bright_scale))
    {
      g_test_skip ("no readable /sys/class/backlight");
      free_qs (qs);
      g_free (bin);
      return;
    }
  g_assert_cmpfloat (gtk_range_get_value (GTK_RANGE (qs->bright_scale)), >=,
                     (gdouble) BRIGHTNESS_MIN);
  g_assert_cmpfloat (gtk_range_get_value (GTK_RANGE (qs->bright_scale)), <=,
                     100.0);

  free_qs (qs);
  g_free (bin);
}

static void
test_sysfs_fake_read (void)
{
  QuickSettings *qs;
  gchar         *bin;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  bin = new_bin_dir ();
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "37", "100");
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 37.0, 2000));
  g_assert_true (gtk_widget_is_sensitive (qs->bright_scale));

  free_qs (qs);
  g_free (sysfs);
  g_free (bin);
}

static void
test_sysfs_pick (void)
{
  QuickSettings *qs;
  gchar         *base;
  gchar         *bin;
  gchar         *dev;

  if (!scenario_ready ())
    return;
  base = g_dir_make_tmp ("qs-bright-pick-XXXXXX", NULL);
  g_assert_nonnull (base);
  dev = g_build_filename (base, "zero", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dev, 0755), ==, 0);
  write_file (dev, "brightness", "10");
  write_file (dev, "max_brightness", "0");
  g_free (dev);
  dev = g_build_filename (base, "acpi_video0", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dev, 0755), ==, 0);
  write_file (dev, "brightness", "50");
  write_file (dev, "max_brightness", "200");
  g_free (dev);
  dev = g_build_filename (base, "intel_backlight", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dev, 0755), ==, 0);
  write_file (dev, "brightness", "25");
  write_file (dev, "max_brightness", "100");
  g_free (dev);
  g_setenv ("QS_BACKLIGHT_DIR", base, TRUE);

  bin = new_bin_dir ();
  set_path (bin, NULL);
  qs = new_qs ();
  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 25.0, 2000));

  free_qs (qs);
  g_free (bin);
  g_free (base);
}

static void
test_sysfs_set (void)
{
  QuickSettings *qs;
  gchar         *bin;
  gchar         *sysfs;
  gchar         *bright_path;

  if (!scenario_ready ())
    return;
  bin = new_bin_dir ();
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "0", "100");
  bright_path = g_build_filename (sysfs, "fakebl", "brightness", NULL);
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 64.0);
  g_assert_true (wait_file (bright_path, "64", 2000));

  free_qs (qs);
  g_free (bright_path);
  g_free (sysfs);
  g_free (bin);
}

static void
test_sysfs_write_fail_xback (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;
  gchar         *sysfs;
  gchar         *sysfs_dir;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_xback);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "0", "100");
  sysfs_dir = g_build_filename (sysfs, "fakebl", NULL);
  g_assert_cmpint (g_chmod (sysfs_dir, 0555), ==, 0);
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  g_assert_true (wait_log (log, "xbacklight -set 70", 2000));

  g_chmod (sysfs_dir, 0755);
  free_qs (qs);
  g_free (sysfs_dir);
  g_free (sysfs);
  g_free (bin);
  g_free (log);
}

static void
test_sysfs_write_fail_none (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;
  gchar         *sysfs;
  gchar         *sysfs_dir;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_xrandr);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "0", "100");
  sysfs_dir = g_build_filename (sysfs, "fakebl", NULL);
  g_assert_cmpint (g_chmod (sysfs_dir, 0555), ==, 0);
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  run_loop_ms (300);
  g_assert_false (log_has (log, "xrandr"));

  g_chmod (sysfs_dir, 0755);
  free_qs (qs);
  g_free (sysfs_dir);
  g_free (sysfs);
  g_free (bin);
  g_free (log);
}

static void
test_sysfs_read_fail (void)
{
  QuickSettings *qs;
  gchar         *bin;
  gchar         *sysfs;
  gchar         *bright_path;

  if (!scenario_ready ())
    return;
  bin = new_bin_dir ();
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  bright_path = g_build_filename (sysfs, "fakebl", "brightness", NULL);
  g_assert_cmpint (g_chmod (bright_path, 0000), ==, 0);
  qs = new_qs ();

  display_refresh_brightness (qs);
  run_loop_ms (300);
  g_assert_false (gtk_widget_is_sensitive (qs->bright_scale));

  g_chmod (bright_path, 0644);
  free_qs (qs);
  g_free (bright_path);
  g_free (sysfs);
  g_free (bin);
}

static void
test_sysfs_reprobe (void)
{
  QuickSettings *qs;
  gchar         *bin;
  gchar         *sysfs;
  gchar         *dev;
  gchar         *bright_path;
  gchar         *max_path;

  if (!scenario_ready ())
    return;
  bin = new_bin_dir ();
  copy_tool (bin, fake_xback);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  qs = new_qs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 50.0, 2000));

  dev = g_build_filename (sysfs, "fakebl", NULL);
  bright_path = g_build_filename (dev, "brightness", NULL);
  max_path = g_build_filename (dev, "max_brightness", NULL);
  g_assert_cmpint (g_remove (bright_path), ==, 0);
  g_assert_cmpint (g_remove (max_path), ==, 0);
  g_assert_cmpint (g_rmdir (dev), ==, 0);
  make_empty_sysfs ();

  display_refresh_brightness (qs);
  g_assert_true (wait_value (qs, 42.0, 2000));

  free_qs (qs);
  g_free (max_path);
  g_free (bright_path);
  g_free (dev);
  g_free (sysfs);
  g_free (bin);
}

/* --- dead/guards/debounce --- */

static void
test_dead_and_null (void)
{
  QuickSettings *qs;

  if (!scenario_ready ())
    return;
  qs = new_qs ();
  qs->dead = TRUE;
  display_refresh_brightness (qs);
  qs->dead = FALSE;
  gtk_widget_destroy (qs->bright_scale);
  qs->bright_scale = NULL;
  display_refresh_brightness (qs);
  g_assert_null (qs->bright_scale);
  qs_unref (qs);
}

static void
test_debounce_guards (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  qs = new_qs ();

  qs->updating = TRUE;
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 80.0);
  run_loop_ms (200);
  qs->updating = FALSE;
  g_assert_false (log_has (log, " set "));

  gtk_widget_set_sensitive (qs->bright_scale, FALSE);
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 90.0);
  run_loop_ms (200);
  g_assert_false (log_has (log, " set "));

  gtk_widget_set_sensitive (qs->bright_scale, TRUE);
  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 70.0);
  bright_stop ();
  bright_stop ();
  run_loop_ms (200);
  g_assert_false (log_has (log, " set "));
  /* bright_stop remove a fonte sem disparar o callback: devolve a ref. */
  if (g_atomic_int_get (&qs->refcount) > 1)
    qs_unref (qs);
  g_assert_cmpint (qs->refcount, ==, 1);

  free_qs (qs);
  g_free (sysfs);
  g_free (bin);
  g_free (log);
}

static void
test_debounce_dead (void)
{
  QuickSettings *qs;
  gchar         *log;
  gchar         *bin;
  gchar         *sysfs;

  if (!scenario_ready ())
    return;
  log = new_log ();
  bin = new_bin_dir ();
  copy_tool (bin, fake_ctl);
  set_path (bin, NULL);
  sysfs = make_sysfs ("fakebl", "50", "100");
  qs = new_qs ();

  gtk_range_set_value (GTK_RANGE (qs->bright_scale), 60.0);
  qs->dead = TRUE;
  run_loop_ms (200);
  g_assert_false (log_has (log, " set "));
  g_assert_cmpint (qs->refcount, ==, 1);

  qs->dead = FALSE;
  free_qs (qs);
  g_free (sysfs);
  g_free (bin);
  g_free (log);
}

int
main (int argc, char **argv)
{
  setlocale (LC_NUMERIC, "C");
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  setlocale (LC_NUMERIC, "C");
  repo_root = g_get_current_dir ();
  fakebin_dir = g_build_filename (repo_root, "tests", "fakebin", NULL);
  fake_ctl = g_build_filename (fakebin_dir, "brightnessctl", NULL);
  fake_xback = g_build_filename (repo_root, "tests", "fakebin-xback",
                                 "xbacklight", NULL);
  fake_xrandr = g_build_filename (repo_root, "tests", "fakebin-xrandr",
                                  "xrandr", NULL);

#define ADD_FLOW(path, fn) \
  g_test_add_func ("/brightness-flow/" path, fn); \
  g_test_add_data_func ("/brightness-flow/" path "-isolated", \
                        "/brightness-flow/" path, run_isolated_data)

  ADD_FLOW ("ctl-refresh", test_ctl_refresh);
  ADD_FLOW ("ctl-second-form", test_ctl_second_form);
  ADD_FLOW ("ctl-fallback-sysfs", test_ctl_fallback_sysfs);
  ADD_FLOW ("ctl-fallback-xback", test_ctl_fallback_xback);
  ADD_FLOW ("ctl-fallback-xrandr", test_ctl_fallback_xrandr);
  ADD_FLOW ("ctl-fallback-none", test_ctl_fallback_none);
  ADD_FLOW ("ctl-fail-sysfs", test_ctl_fail_sysfs);
  ADD_FLOW ("ctl-invalid-percent", test_ctl_invalid_percent);
  ADD_FLOW ("ctl-dead-callback", test_ctl_dead_callback);
  ADD_FLOW ("xback-refresh-set", test_xback_refresh_set);
  ADD_FLOW ("xback-bad-to-xrandr", test_xback_bad_to_xrandr);
  ADD_FLOW ("xback-bad-disable", test_xback_bad_disable);
  ADD_FLOW ("xrandr-refresh-set", test_xrandr_refresh_set);
  ADD_FLOW ("xrandr-set-miss", test_xrandr_set_miss);
  ADD_FLOW ("ctl-set-dev", test_ctl_set_dev);
  ADD_FLOW ("ctl-set-no-dev", test_ctl_set_no_dev);
  ADD_FLOW ("sysfs-real-read", test_sysfs_real_read);
  ADD_FLOW ("sysfs-fake-read", test_sysfs_fake_read);
  ADD_FLOW ("sysfs-pick", test_sysfs_pick);
  ADD_FLOW ("sysfs-set", test_sysfs_set);
  ADD_FLOW ("sysfs-write-fail-xback", test_sysfs_write_fail_xback);
  ADD_FLOW ("sysfs-write-fail-none", test_sysfs_write_fail_none);
  ADD_FLOW ("sysfs-read-fail", test_sysfs_read_fail);
  ADD_FLOW ("sysfs-reprobe", test_sysfs_reprobe);
  ADD_FLOW ("dead-and-null", test_dead_and_null);
  ADD_FLOW ("debounce-guards", test_debounce_guards);
  ADD_FLOW ("debounce-dead", test_debounce_dead);

  return g_test_run ();
}
