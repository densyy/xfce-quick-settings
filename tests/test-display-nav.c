/* test-display-nav.c — navegação dos tiles Display/Night Light (qs-display.c).
 *
 * Constrói a seção Display completa e emite "clicked" nos tiles
 * (on_night_tile_clicked / on_display_tile_clicked) e nos botões qs-back
 * das páginas "night"/"display" (on_night_back_clicked /
 * on_display_back_clicked), validando o stack visível e o subtítulo do
 * nightlight.
 *
 * PATH contém SOMENTE tests/fakebin: xrandr/redshift falsos; nenhuma
 * ferramenta real é executada (QS_FAKE_LOG prova os argv chamados).
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-display.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
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
log_has (const gchar *needle)
{
  gchar   *text = NULL;
  gboolean hit;

  g_file_get_contents (log_path, &text, NULL, NULL);
  hit = (text != NULL && strstr (text, needle) != NULL);
  g_free (text);
  return hit;
}

static gboolean
wait_log (const gchar *needle,
          guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_has (needle))
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return log_has (needle);
}

static gboolean
wait_label_has (GtkWidget   *label,
                const gchar *needle,
                guint        timeout_ms)
{
  guint        waited = 0;
  const gchar *text;

  while (waited < timeout_ms)
    {
      text = gtk_label_get_text (GTK_LABEL (label));
      if (text != NULL && strstr (text, needle) != NULL)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  text = gtk_label_get_text (GTK_LABEL (label));
  return (text != NULL && strstr (text, needle) != NULL);
}

/* Acha o GtkButton de classe "qs-back" dentro de uma página do stack. */
static GtkWidget *
find_back_button (GtkWidget *root)
{
  GtkStyleContext *ctx;

  if (!GTK_IS_WIDGET (root))
    return NULL;
  if (GTK_IS_BUTTON (root))
    {
      ctx = gtk_widget_get_style_context (root);
      if (gtk_style_context_has_class (ctx, "qs-back"))
        return root;
    }
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_back_button (GTK_WIDGET (l->data));
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static gboolean
visible_is (GtkWidget   *stack,
            const gchar *name)
{
  return g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (stack)),
                    name) == 0;
}

/* --- night light: tile + voltar --- */

static void
test_night_tile_and_back (void)
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
  GtkWidget     *page;
  GtkWidget     *back;

  if (skip_no_display ())
    return;

  g_remove (log_path);

  qs = qs_new ();
  popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->popup = popup;
  stack = gtk_stack_new ();
  qs->stack = stack;
  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_NONE);
  main_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (stack), main_page, "main");
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  display_build_section (box, stack, qs);
  gtk_widget_show_all (box);
  gtk_widget_show_all (stack);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "main");

  g_assert_true (GTK_IS_BUTTON (qs->night_tile));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Off");

  /* on_night_tile_clicked: página "night". */
  g_signal_emit_by_name (qs->night_tile, "clicked");
  g_assert_true (visible_is (stack, "night"));

  /* Preset -> subtítulo "Day" (redshift falso, logado). */
  g_assert_true (night_available ());
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (qs->night_day), TRUE);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Day");
  g_assert_true (wait_log ("redshift -P -O 6500K", 2000));

  /* on_night_back_clicked: night_update_sub + "main". */
  page = gtk_stack_get_child_by_name (GTK_STACK (stack), "night");
  g_assert_nonnull (page);
  back = find_back_button (page);
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  g_assert_true (visible_is (stack, "main"));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->night_sub)), ==, "Day");

  run_loop_ms (200);
  gtk_widget_destroy (box);
  gtk_widget_destroy (stack);
  gtk_widget_destroy (popup);
  qs_unref (qs);
}

/* --- display: tile + voltar --- */

static void
test_display_tile_and_back (void)
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
  GtkWidget     *page;
  GtkWidget     *back;

  if (skip_no_display ())
    return;

  g_remove (log_path);

  qs = qs_new ();
  popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->popup = popup;
  stack = gtk_stack_new ();
  qs->stack = stack;
  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_NONE);
  main_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (stack), main_page, "main");
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  display_build_section (box, stack, qs);
  gtk_widget_show_all (box);
  gtk_widget_show_all (stack);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "main");

  g_assert_true (GTK_IS_BUTTON (qs->display_tile));

  /* on_display_tile_clicked: display_load_async (xrandr falso) + página. */
  g_signal_emit_by_name (qs->display_tile, "clicked");
  g_assert_true (visible_is (stack, "display"));
  g_assert_true (wait_log ("xrandr --query", 3000));
  g_assert_true (wait_label_has (qs->display_sub, "HDMI-1", 3000));

  /* on_display_back_clicked: show_main_page. */
  page = gtk_stack_get_child_by_name (GTK_STACK (stack), "display");
  g_assert_nonnull (page);
  back = find_back_button (page);
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  g_assert_true (visible_is (stack, "main"));

  run_loop_ms (200);
  gtk_widget_destroy (box);
  gtk_widget_destroy (stack);
  gtk_widget_destroy (popup);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *cwd;

  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-display-nav-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);

  /* PATH só com fakes: xrandr/redshift reais nunca são chamados. */
  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  g_free (cwd);
  g_assert_true (g_file_test (fakebin, G_FILE_TEST_IS_DIR));
  g_setenv ("PATH", fakebin, TRUE);
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");

  log_path = g_build_filename (tmp_root, "fake-argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log_path, TRUE);

  g_test_add_func ("/display/night_tile_and_back", test_night_tile_and_back);
  g_test_add_func ("/display/display_tile_and_back",
                   test_display_tile_and_back);

  return g_test_run ();
}
