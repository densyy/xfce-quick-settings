/* test-audio-amixer.c — fluxos de audio com backend NONE + amixer.
 *
 * PATH contem SOMENTE um dir fake com amixer (copia de tests/fakebin/amixer),
 * entao o probe cacheado por processo resolve AUDIO_NONE com amixer
 * disponivel. Nenhum amixer real e executado. Precisa de display.
 */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-audio.h"
#include "qs-audio-backend.h"

void audio_refresh_input_volume (QuickSettings *qs);

static gboolean have_display = FALSE;
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

static gboolean
log_contains (const gchar *needle)
{
  gchar   *text = NULL;
  gboolean hit = FALSE;

  if (g_file_get_contents (log_path, &text, NULL, NULL))
    {
      hit = strstr (text, needle) != NULL;
      g_free (text);
    }
  return hit;
}

static gboolean
wait_for_log (const gchar *needle,
              gint         timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_contains (needle))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return log_contains (needle);
}

static gboolean
wait_for_range (GtkRange *range,
                gdouble   want,
                gint      timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (ABS (gtk_range_get_value (range) - want) < 0.5)
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return ABS (gtk_range_get_value (range) - want) < 0.5;
}

static void
init_widgets (QuickSettings *qs)
{
  qs->vol_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
  qs->vol_icon = gtk_image_new ();
  qs->audio_out_vol_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                      0, 150, 1);
  qs->audio_out_vol_icon = gtk_image_new ();
  qs->audio_in_vol_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                     0, 150, 1);
  qs->audio_in_vol_icon = gtk_image_new ();
}

/* --- probe: sem pactl/wpctl, com amixer --- */

static void
test_amixer_probe (void)
{
  g_assert_cmpint (audio_get_backend_cached (), ==, AUDIO_NONE);
  g_assert_true (audio_have_amixer_avail ());
}

/* --- leitura de volume via amixer --- */

static void
test_amixer_refresh_volume (void)
{
  QuickSettings *qs;

  if (skip_no_display ())
    return;
  qs = qs_new ();
  init_widgets (qs);

  audio_refresh_volume (qs);
  g_assert_true (wait_for_range (GTK_RANGE (qs->audio_out_vol_scale), 50, 5000));
  g_assert_cmpstr (gtk_widget_get_tooltip_text (qs->audio_out_vol_scale),
                   ==, "Volume");
  audio_refresh_input_volume (qs);
  g_assert_true (wait_for_range (GTK_RANGE (qs->audio_in_vol_scale), 50, 5000));
  g_assert_true (wait_for_log ("amixer get Master", 5000));
  g_assert_true (wait_for_log ("amixer get Capture", 5000));

  qs_unref (qs);
}

/* --- escrita de volume (clamp em 100) e dispositivos indisponiveis --- */

static void
test_amixer_controls_and_devices (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    return;
  qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);
  qs->audio_backend = AUDIO_NONE;

  audio_refresh_devices (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->audio_sub)),
                   ==, "No audio backend");
  g_assert_false (gtk_widget_is_sensitive (qs->sink_combo));
  g_assert_false (gtk_widget_is_sensitive (qs->source_combo));
  gtk_combo_box_set_active (GTK_COMBO_BOX (qs->sink_combo), 0);
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->sink_combo)),
                   ==, "none");

  gtk_range_set_value (GTK_RANGE (qs->vol_scale), 120);
  g_assert_true (wait_for_log ("amixer -q sset Master 100%", 5000));
  gtk_range_set_value (GTK_RANGE (qs->audio_out_vol_scale), 60);
  g_assert_true (wait_for_log ("amixer -q sset Master 60%", 5000));
  gtk_range_set_value (GTK_RANGE (qs->audio_in_vol_scale), 75);
  g_assert_true (wait_for_log ("amixer -q sset Capture 75%", 5000));

  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gchar *src;
  gchar *bindir;
  gchar *dst;
  gchar *script = NULL;
  gsize  length = 0;
  gint   ret;

  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);

  cwd = g_get_current_dir ();
  src = g_build_filename (cwd, "tests", "fakebin", "amixer", NULL);
  g_assert_true (g_file_get_contents (src, &script, &length, NULL));
  bindir = g_dir_make_tmp ("qs-test-audio-amixer-bin-XXXXXX", NULL);
  g_assert_nonnull (bindir);
  dst = g_build_filename (bindir, "amixer", NULL);
  g_assert_true (g_file_set_contents (dst, script, (gssize) length, NULL));
  g_chmod (dst, 0755);
  log_path = g_build_filename (bindir, "amixer.log", NULL);
  g_setenv ("PATH", bindir, TRUE);
  g_setenv ("QS_FAKE_LOG", log_path, TRUE);

  g_test_add_func ("/audio-amixer/probe", test_amixer_probe);
  g_test_add_func ("/audio-amixer/refresh_volume", test_amixer_refresh_volume);
  g_test_add_func ("/audio-amixer/controls_and_devices",
                   test_amixer_controls_and_devices);

  ret = g_test_run ();
  g_remove (dst);
  g_remove (log_path);
  g_rmdir (bindir);
  g_free (dst);
  g_free (bindir);
  g_free (src);
  g_free (script);
  g_free (cwd);
  return ret;
}
