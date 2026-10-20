/* test-nightlight.c — presets, reset e fallback redshift/gammastep.
 *
 * PATH sempre restrito aos fakes (nunca chama redshift/gammastep reais).
 * Precisa de display para os widgets; pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-display.h"

static gboolean have_display = FALSE;
static gchar   *repo_root = NULL;
static gchar   *fakebin_dir = NULL;
static gchar   *gammastep_dir = NULL;

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

static gchar *
new_log (void)
{
  gchar *dir = g_dir_make_tmp ("qs-night-log-XXXXXX", NULL);
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
set_path (const gchar *path)
{
  g_setenv ("PATH", path, TRUE);
}

static QuickSettings *
build_section (GtkWidget **box_out,
               GtkWidget **stack_out)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  GtkWidget     *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget     *stack = gtk_stack_new ();

  qs_ref (qs);
  qs->stack = stack;
  display_build_section (box, stack, qs);
  *box_out = box;
  *stack_out = stack;
  return qs;
}

static void
destroy_section (QuickSettings *qs,
                 GtkWidget     *box,
                 GtkWidget     *stack)
{
  gtk_widget_destroy (stack);
  gtk_widget_destroy (box);
  qs->stack = NULL;
  qs->bright_scale = NULL;
  qs->night_sub = NULL;
  qs->night_day = NULL;
  qs->night_aft = NULL;
  qs->night_night = NULL;
  qs->night_dawn = NULL;
  qs_unref (qs);
}

/* Widgets sem pai mantem a ref flutuante: sink, destruir e soltar a ref. */
static void
destroy_widget (GtkWidget *widget)
{
  g_object_ref_sink (widget);
  gtk_widget_destroy (widget);
  g_object_unref (widget);
}

static void
test_presets (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *log;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  log = new_log ();
  set_path (fakebin_dir);
  qs = build_section (&box, &stack);

  g_assert_true (night_available ());
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Off");
  g_assert_true (GTK_IS_TOGGLE_BUTTON (qs->night_day));
  g_assert_true (GTK_IS_TOGGLE_BUTTON (qs->night_aft));
  g_assert_true (GTK_IS_TOGGLE_BUTTON (qs->night_night));
  g_assert_true (GTK_IS_TOGGLE_BUTTON (qs->night_dawn));
  g_assert_cmpstr (g_object_get_data (G_OBJECT (qs->night_day), "qs-temp"), ==,
                   "6500K");
  g_assert_cmpstr (g_object_get_data (G_OBJECT (qs->night_aft), "qs-temp"), ==,
                   "5500K");
  g_assert_cmpstr (g_object_get_data (G_OBJECT (qs->night_night),
                                      "qs-temp"), ==, "4500K");
  g_assert_cmpstr (g_object_get_data (G_OBJECT (qs->night_dawn),
                                      "qs-temp"), ==, "3500K");

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), TRUE);
  g_assert_true (wait_log (log, "redshift -P -O 6500K", 2000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Day");
  g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_aft)));

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_aft), TRUE);
  g_assert_true (wait_log (log, "redshift -P -O 5500K", 2000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==,
                   "Afternoon");
  g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_day)));

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_night), TRUE);
  g_assert_true (wait_log (log, "redshift -P -O 4500K", 2000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Night");

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_dawn), TRUE);
  g_assert_true (wait_log (log, "redshift -P -O 3500K", 2000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Dawn");

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_dawn), FALSE);
  g_assert_true (wait_log (log, "redshift -x", 2000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Off");

  destroy_section (qs, box, stack);
  g_free (log);
}

static void
test_updating_guard (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *log;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  log = new_log ();
  set_path (fakebin_dir);
  qs = build_section (&box, &stack);

  qs->updating = TRUE;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), TRUE);
  run_loop_ms (200);
  g_assert_false (log_has (log, "6500K"));

  qs->updating = FALSE;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), FALSE);
  g_assert_true (wait_log (log, "redshift -x", 2000));

  destroy_section (qs, box, stack);
  g_free (log);
}

static void
test_gammastep (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *log;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  log = new_log ();
  set_path (gammastep_dir);
  qs = build_section (&box, &stack);

  g_assert_true (night_available ());
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), TRUE);
  g_assert_true (wait_log (log, "gammastep -P -O 6500K", 2000));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), FALSE);
  g_assert_true (wait_log (log, "gammastep -x", 2000));
  g_assert_false (log_has (log, "redshift"));

  destroy_section (qs, box, stack);
  g_free (log);
}

static void
test_unavailable (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *empty;
  gchar         *log;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  empty = g_dir_make_tmp ("qs-night-empty-XXXXXX", NULL);
  g_assert_nonnull (empty);
  log = new_log ();
  set_path (empty);
  qs = build_section (&box, &stack);

  g_assert_false (night_available ());
  g_assert_false (gtk_widget_is_sensitive (qs->night_day));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_night), TRUE);
  run_loop_ms (200);
  g_assert_false (log_has (log, "redshift"));
  g_assert_false (log_has (log, "gammastep"));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Night");

  destroy_section (qs, box, stack);
  g_free (log);
  g_free (empty);
}

static void
test_update_sub_edges (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  GtkWidget     *label = gtk_label_new ("x");
  GtkWidget     *aft = gtk_toggle_button_new ();
  GtkWidget     *night = gtk_toggle_button_new ();
  GtkWidget     *dawn = gtk_toggle_button_new ();

  night_update_sub (qs);

  qs->night_sub = label;
  night_update_sub (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "Off");

  qs->night_aft = aft;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (aft), TRUE);
  night_update_sub (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "Afternoon");

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (aft), FALSE);
  qs->night_night = night;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (night), TRUE);
  night_update_sub (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "Night");

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (night), FALSE);
  qs->night_dawn = dawn;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dawn), TRUE);
  night_update_sub (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "Dawn");

  destroy_widget (label);
  destroy_widget (aft);
  destroy_widget (night);
  destroy_widget (dawn);
  g_free (qs);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  repo_root = g_get_current_dir ();
  fakebin_dir = g_build_filename (repo_root, "tests", "fakebin", NULL);
  gammastep_dir = g_build_filename (repo_root, "tests", "fakebin-gammastep",
                                    NULL);

  g_test_add_func ("/nightlight/presets", test_presets);
  g_test_add_func ("/nightlight/updating-guard", test_updating_guard);
  g_test_add_func ("/nightlight/gammastep", test_gammastep);
  g_test_add_func ("/nightlight/unavailable", test_unavailable);
  g_test_add_func ("/nightlight/update-sub-edges", test_update_sub_edges);

  return g_test_run ();
}
