/* test-plugin.c — núcleo testável do plugin: construção, toggle, fade,
 * size-changed e teardown.
 *
 * O container é uma GtkWindow invisível e o popup fica com opacidade
 * zero, então nada aparece no desktop. PATH é prefixado com
 * tests/fakebin e o log QS_FAKE_LOG prova que nenhuma ferramenta real
 * foi chamada.
 * Sem display os casos dão skip.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-plugin.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;

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

/* Build núcleo em um container real e invisível (opacidade 0, sem foco):
 * o popup criado pelo plugin também é deixado transparente. */
static QuickSettings *
build_plugin (GtkWidget **container_out)
{
  GtkWidget     *container;
  QuickSettings *qs = NULL;
  GdkMonitor    *monitor;
  GdkRectangle   area;

  monitor = gdk_display_get_monitor (gdk_display_get_default (), 0);
  g_assert_nonnull (monitor);
  gdk_monitor_get_workarea (monitor, &area);

  container = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated (GTK_WINDOW (container), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (container), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (container), TRUE);
  gtk_window_set_accept_focus (GTK_WINDOW (container), FALSE);
  gtk_window_set_focus_on_map (GTK_WINDOW (container), FALSE);
  gtk_widget_set_opacity (container, 0.0);
  gtk_widget_set_size_request (container, 420, 320);
  gtk_window_move (GTK_WINDOW (container), area.x + 2, area.y + 2);
  qs_plugin_build (container, &qs);
  g_assert_nonnull (qs);
  gtk_widget_show_all (container);
  run_loop_ms (100);

  gtk_window_set_accept_focus (GTK_WINDOW (qs->popup), FALSE);
  gtk_window_set_focus_on_map (GTK_WINDOW (qs->popup), FALSE);
  gtk_widget_set_opacity (qs->popup, 0.0);

  *container_out = container;
  return qs;
}

static void
destroy_plugin (QuickSettings *qs,
                GtkWidget     *container)
{
  qs_plugin_teardown (qs);
  gtk_widget_destroy (container);
  run_loop_ms (600);
}

static gchar *
make_fake_log (const gchar *name)
{
  gchar *dir = g_dir_make_tmp (name, NULL);
  gchar *log;

  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_free (dir);
  return log;
}

/* QS_VERSION segue semver (MAJOR.MINOR.PATCH); ver AGENTS.md. */
static void
test_version_format (void)
{
  g_assert_true (g_regex_match_simple ("^[0-9]+\\.[0-9]+\\.[0-9]+$",
                                       QS_VERSION, 0, 0));
}

static GtkWidget *
find_by_class (GtkWidget   *root,
               const gchar *name)
{
  if (!GTK_IS_WIDGET (root))
    return NULL;
  if (gtk_style_context_has_class (gtk_widget_get_style_context (root),
                                   name))
    return root;
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_by_class (GTK_WIDGET (l->data), name);
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static void
test_build_structure (void)
{
  QuickSettings *qs;
  GtkWidget     *container = NULL;
  GtkWidget     *main_box;
  gchar         *log;
  gchar         *contents = NULL;
  const gchar   *pages[] = { "audio", "display", "night", "wifi", "bt",
                             "sysmon", "notices", "power" };
  guint          i;

  if (skip_no_display ())
    return;

  log = make_fake_log ("qs-test-plugin-build-XXXXXX");
  qs = build_plugin (&container);
  run_loop_ms (500);

  g_assert_true (GTK_IS_BUTTON (qs->button));
  g_assert_true (GTK_IS_IMAGE (qs->button_image));
  g_assert_true (GTK_IS_WINDOW (qs->popup));
  g_assert_true (GTK_IS_STACK (qs->stack));
  g_assert_true (gtk_widget_get_parent (qs->button) == container);
  g_assert_true (gtk_style_context_has_class (
                   gtk_widget_get_style_context (qs->popup), "qs-popup"));
  g_assert_false (gtk_widget_get_visible (qs->popup));

  main_box = gtk_stack_get_child_by_name (GTK_STACK (qs->stack), "main");
  g_assert_nonnull (main_box);
  g_assert_true (GTK_IS_BOX (main_box));
  for (i = 0; i < G_N_ELEMENTS (pages); i++)
    g_assert_nonnull (gtk_stack_get_child_by_name (GTK_STACK (qs->stack),
                                                   pages[i]));

  g_assert_nonnull (qs->vol_scale);
  g_assert_nonnull (qs->bright_scale);
  g_assert_nonnull (qs->wifi_tile);
  g_assert_nonnull (qs->bt_tile);
  g_assert_nonnull (qs->display_tile);
  g_assert_nonnull (qs->notify_list);
  g_assert_nonnull (qs->sysmon_ram_value);
  g_assert_nonnull (qs->bat_label);

  /* Version tag discreta no canto inferior direito da página main. */
  {
    gchar     *expected = g_strdup_printf ("v%s", QS_VERSION);
    GtkWidget *version = find_by_class (main_box, "qs-version");

    g_assert_nonnull (version);
    g_assert_true (GTK_IS_LABEL (version));
    g_assert_true (version == qs->version_label);
    g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (version)), ==, expected);
    g_assert_cmpint (gtk_widget_get_halign (version), ==, GTK_ALIGN_END);
    g_free (expected);
  }

  g_assert_true (g_file_get_contents (log, &contents, NULL, NULL));
  g_assert_nonnull (g_strstr_len (contents, -1, "pactl"));
  g_free (contents);
  g_free (log);

  destroy_plugin (qs, container);
}

static void
test_toggle_and_fade (void)
{
  QuickSettings *qs;
  GtkWidget     *container = NULL;
  GtkWidget     *main_box;
  guint          i;

  if (skip_no_display ())
    return;

  qs = build_plugin (&container);
  main_box = gtk_stack_get_child_by_name (GTK_STACK (qs->stack), "main");
  g_assert_nonnull (main_box);

  g_signal_emit_by_name (qs->button, "clicked");
  g_assert_true (gtk_widget_get_visible (qs->popup));
  g_assert_cmpuint (qs->fade_tick, !=, 0);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (qs->stack)),
                   ==, "main");

  for (i = 0; i < 300 && qs->fade_tick != 0; i++)
    run_loop_ms (10);
  g_assert_cmpuint (qs->fade_tick, ==, 0);
  g_assert_cmpfloat (gtk_widget_get_opacity (main_box), ==, 1.0);

  g_signal_emit_by_name (qs->button, "clicked");
  g_assert_false (gtk_widget_get_visible (qs->popup));
  g_assert_cmpfloat (gtk_widget_get_opacity (main_box), ==, 1.0);

  /* Reabre e esconde no meio do fade: cancelamento no sinal hide. */
  g_signal_emit_by_name (qs->button, "clicked");
  g_assert_true (gtk_widget_get_visible (qs->popup));
  g_assert_cmpuint (qs->fade_tick, !=, 0);
  gtk_widget_hide (qs->popup);
  g_assert_cmpuint (qs->fade_tick, ==, 0);
  g_assert_cmpfloat (gtk_widget_get_opacity (main_box), ==, 1.0);
  g_assert_false (gtk_widget_get_visible (qs->popup));

  /* Dead durante o fade: o tick se remove sozinho. */
  g_signal_emit_by_name (qs->button, "clicked");
  g_assert_true (gtk_widget_get_visible (qs->popup));
  g_atomic_int_set (&qs->dead, TRUE);
  for (i = 0; i < 300 && qs->fade_tick != 0; i++)
    run_loop_ms (10);
  g_assert_cmpuint (qs->fade_tick, ==, 0);
  g_atomic_int_set (&qs->dead, FALSE);
  gtk_widget_hide (qs->popup);

  destroy_plugin (qs, container);
}

static void
test_size_changed (void)
{
  QuickSettings *qs;
  GtkWidget     *container = NULL;
  gint           width = 0;
  gint           height = 0;

  if (skip_no_display ())
    return;

  qs = build_plugin (&container);

  g_assert_true (qs_plugin_size_changed (qs, container, 44, 24, 1));
  gtk_widget_get_size_request (qs->button, &width, &height);
  g_assert_cmpint (width, ==, 44);
  g_assert_cmpint (height, ==, 44);
  g_assert_cmpint (gtk_image_get_pixel_size (GTK_IMAGE (qs->button_image)),
                   ==, 24);
  g_assert_cmpuint (gtk_container_get_border_width (GTK_CONTAINER (container)),
                    ==, 2);

  g_assert_true (qs_plugin_size_changed (qs, container, 44, 24, 2));
  gtk_widget_get_size_request (qs->button, &width, &height);
  g_assert_cmpint (width, ==, 22);
  g_assert_cmpint (height, ==, 22);
  g_assert_cmpuint (gtk_container_get_border_width (GTK_CONTAINER (container)),
                    ==, 2);

  g_assert_true (qs_plugin_size_changed (qs, container, 20, 8, 1));
  gtk_widget_get_size_request (qs->button, &width, &height);
  g_assert_cmpint (width, ==, 20);
  g_assert_cmpint (height, ==, 20);
  g_assert_cmpint (gtk_image_get_pixel_size (GTK_IMAGE (qs->button_image)),
                   ==, 16);
  g_assert_cmpuint (gtk_container_get_border_width (GTK_CONTAINER (container)),
                    ==, 1);

  g_assert_true (qs_plugin_size_changed (qs, NULL, 30, 20, 1));

  destroy_plugin (qs, container);
}

static gboolean
unused_timer (gpointer data)
{
  (void) data;
  return G_SOURCE_CONTINUE;
}

static void
on_popup_destroyed (GtkWidget *widget,
                    gpointer   user_data)
{
  gboolean *flag = user_data;

  (void) widget;
  *flag = TRUE;
}

static void
test_teardown (void)
{
  QuickSettings *qs;
  GtkWidget     *container = NULL;
  gboolean       destroyed = FALSE;
  guint          i;

  if (skip_no_display ())
    return;

  qs = build_plugin (&container);
  g_signal_emit_by_name (qs->button, "clicked");
  g_assert_true (gtk_widget_get_visible (qs->popup));
  run_loop_ms (700);

  qs->sysmon_timer = g_timeout_add_seconds (60, unused_timer, NULL);
  qs->power_timer = g_timeout_add_seconds (60, unused_timer, NULL);
  g_signal_connect (qs->popup, "destroy", G_CALLBACK (on_popup_destroyed),
                    &destroyed);

  qs_ref (qs);
  qs_plugin_teardown (qs);

  g_assert_true (g_atomic_int_get (&qs->dead));
  g_assert_cmpuint (qs->fade_tick, ==, 0);
  g_assert_cmpuint (qs->sysmon_timer, ==, 0);
  g_assert_cmpuint (qs->power_timer, ==, 0);
  g_assert_true (destroyed);

  for (i = 0; i < 600 && g_atomic_int_get (&qs->refcount) > 1; i++)
    run_loop_ms (10);
  g_assert_cmpint (g_atomic_int_get (&qs->refcount), ==, 1);

  qs_unref (qs);
  gtk_widget_destroy (container);
  run_loop_ms (200);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gint   ret;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-plugin-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  {
    const gchar *old_path = g_getenv ("PATH");
    gchar       *prefixed = g_strdup_printf ("%s:%s", fakebin,
                                             old_path != NULL
                                             ? old_path : "/usr/bin:/bin");

    g_setenv ("PATH", prefixed, TRUE);
    g_free (prefixed);
  }
  g_free (cwd);

  g_test_add_func ("/plugin/version_format", test_version_format);
  g_test_add_func ("/plugin/build_structure", test_build_structure);
  g_test_add_func ("/plugin/toggle_and_fade", test_toggle_and_fade);
  g_test_add_func ("/plugin/size_changed", test_size_changed);
  g_test_add_func ("/plugin/teardown", test_teardown);

  ret = g_test_run ();
  g_free (fakebin);
  g_free (tmp_root);
  return ret;
}
