/* test-audio-flow.c — fluxos de audio com backend pactl (fakebin).
 *
 * Cobre refresh de volume/saida/entrada, lista de dispositivos,
 * troca de default (sink/source), sliders, navegacao do tile e voltar.
 * PATH e prefixado com tests/fakebin; nenhum pactl real e executado.
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-audio.h"
#include "qs-audio-backend.h"

void audio_refresh_input_volume (QuickSettings *qs);

static gboolean have_display = FALSE;

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
make_fake_dir (void)
{
  gchar *dir = g_dir_make_tmp ("qs-test-audio-flow-XXXXXX", NULL);

  g_assert_nonnull (dir);
  return dir;
}

static gboolean
log_contains (const gchar *path,
              const gchar *needle)
{
  gchar   *text = NULL;
  gboolean hit = FALSE;

  if (g_file_get_contents (path, &text, NULL, NULL))
    {
      hit = strstr (text, needle) != NULL;
      g_free (text);
    }
  return hit;
}

static gboolean
wait_for_log (const gchar *path,
              const gchar *needle,
              gint         timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_contains (path, needle))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return log_contains (path, needle);
}

static gboolean
wait_for_range (GtkRange *range,
                gdouble   want,
                gint      timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      gdouble value = gtk_range_get_value (range);

      if (ABS (value - want) < 0.5)
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return ABS (gtk_range_get_value (range) - want) < 0.5;
}

static gboolean
wait_for_combo_id (GtkComboBox *combo,
                   const gchar *id,
                   gint         timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (g_strcmp0 (gtk_combo_box_get_active_id (combo), id) == 0)
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return g_strcmp0 (gtk_combo_box_get_active_id (combo), id) == 0;
}

static gboolean
wait_for_insensitive (GtkWidget *widget,
                      gint       timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (!gtk_widget_is_sensitive (widget))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return !gtk_widget_is_sensitive (widget);
}

static GtkWidget *
new_scale (void)
{
  return gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
}

static const gchar *
image_icon_name (GtkWidget *widget)
{
  const gchar *name = NULL;

  gtk_image_get_icon_name (GTK_IMAGE (widget), &name, NULL);
  return name;
}

static gboolean
has_label_text (GtkWidget   *widget,
                const gchar *text)
{
  if (GTK_IS_LABEL (widget))
    return g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text) == 0;
  if (GTK_IS_CONTAINER (widget))
    {
      GList   *kids = gtk_container_get_children (GTK_CONTAINER (widget));
      GList   *l;
      gboolean hit = FALSE;

      for (l = kids; l != NULL && !hit; l = l->next)
        hit = has_label_text (GTK_WIDGET (l->data), text);
      g_list_free (kids);
      return hit;
    }
  return FALSE;
}

static GtkWidget *
find_button_with_label (GtkWidget   *root,
                        const gchar *text)
{
  if (GTK_IS_BUTTON (root) && has_label_text (root, text))
    return root;
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_button_with_label (GTK_WIDGET (l->data), text);
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static GtkWidget *
find_first_button (GtkWidget *root)
{
  if (GTK_IS_BUTTON (root))
    return root;
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_first_button (GTK_WIDGET (l->data));
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static void
write_fake_pactl (const gchar *dir)
{
  const gchar *script =
    "#!/bin/sh\n"
    "if [ -n \"${QS_FAKE_LOG:-}\" ]; then printf 'pactl %s\\n' \"$*\" >> \"$QS_FAKE_LOG\"; fi\n"
    "case \"$*\" in\n"
    "  \"get-sink-volume @DEFAULT_SINK@\"|\"get-source-volume @DEFAULT_SOURCE@\")\n"
    "    printf '%s\\n' \"${QS_FAKE_VOLUME:-Volume: front-left: 32768 / 50%% / -18.06 dB}\" ;;\n"
    "esac\n"
    "exit 0\n";
  gchar *path = g_build_filename (dir, "pactl", NULL);

  g_file_set_contents (path, script, -1, NULL);
  g_chmod (path, 0755);
  g_free (path);
}

/* --- refresh de volume e dispositivos --- */

static void
test_flow_refresh (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *dir;
  gchar         *log;

  if (skip_no_display ())
    return;
  dir = make_fake_dir ();
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);

  audio_refresh_volume (qs);
  audio_refresh_input_volume (qs);
  g_assert_true (wait_for_range (GTK_RANGE (qs->audio_out_vol_scale), 50, 5000));
  g_assert_true (wait_for_range (GTK_RANGE (qs->audio_in_vol_scale), 50, 5000));
  assert_tooltip (qs->audio_out_vol_scale, "Volume");
  g_assert_cmpstr (image_icon_name (qs->audio_out_vol_icon),
                   ==, "audio-volume-medium-symbolic");
  g_assert_cmpstr (image_icon_name (qs->audio_in_vol_icon),
                   ==, "audio-volume-medium-symbolic");
  g_assert_true (log_contains (log, "get-sink-volume @DEFAULT_SINK@"));
  g_assert_true (log_contains (log, "get-source-volume @DEFAULT_SOURCE@"));

  audio_refresh_devices (qs);
  g_assert_true (wait_for_combo_id (GTK_COMBO_BOX (qs->sink_combo),
                                    "alsa_output.pci-0", 5000));
  g_assert_true (wait_for_combo_id (GTK_COMBO_BOX (qs->source_combo),
                                    "alsa_input.pci-0", 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->audio_sub)),
                   ==, "Built-in Audio");
  g_assert_true (gtk_widget_is_sensitive (qs->sink_combo));
  g_assert_true (gtk_widget_is_sensitive (qs->source_combo));

  gtk_range_set_value (GTK_RANGE (qs->audio_out_vol_scale), 55);
  g_assert_true (wait_for_log (log, "set-sink-volume @DEFAULT_SINK@ 55%", 5000));
  g_assert_cmpstr (image_icon_name (qs->audio_out_vol_icon),
                   ==, "audio-volume-medium-symbolic");
  gtk_range_set_value (GTK_RANGE (qs->audio_out_vol_scale), 120);
  g_assert_true (wait_for_log (log, "set-sink-volume @DEFAULT_SINK@ 120%", 5000));
  assert_tooltip (qs->audio_out_vol_scale, "Volume (boosted above 100%)");
  g_assert_cmpstr (image_icon_name (qs->audio_out_vol_icon),
                   ==, "audio-volume-overamplified-symbolic");

  gtk_range_set_value (GTK_RANGE (qs->audio_in_vol_scale), 45);
  g_assert_true (wait_for_log (log, "set-source-volume @DEFAULT_SOURCE@ 45%", 5000));
  assert_tooltip (qs->audio_in_vol_scale, "Input volume");
  g_assert_cmpstr (image_icon_name (qs->audio_in_vol_icon),
                   ==, "audio-volume-medium-symbolic");

  gtk_range_set_value (GTK_RANGE (qs->vol_scale), 34);
  g_assert_true (wait_for_log (log, "set-sink-volume @DEFAULT_SINK@ 34%", 5000));
  g_assert_cmpstr (image_icon_name (qs->vol_icon),
                   ==, "audio-volume-medium-symbolic");
  gtk_range_set_value (GTK_RANGE (qs->vol_scale), 130);
  g_assert_true (wait_for_log (log, "set-sink-volume @DEFAULT_SINK@ 130%", 5000));
  assert_tooltip (qs->vol_scale, "Volume (boosted above 100%)");

  qs->updating = TRUE;
  gtk_range_set_value (GTK_RANGE (qs->vol_scale), 20);
  qs->updating = FALSE;
  run_loop_ms (200);
  g_assert_false (log_contains (log, "set-sink-volume @DEFAULT_SINK@ 20%"));

  qs_unref (qs);
  g_remove (log);
  g_rmdir (dir);
  g_free (log);
  g_free (dir);
}

/* --- troca de dispositivos default --- */

static void
test_flow_device_changes (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  gchar         *dir;
  gchar         *log;

  if (skip_no_display ())
    return;
  dir = make_fake_dir ();
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);
  qs->audio_backend = AUDIO_PACTL;

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->sink_combo),
                             "bluez_output.AB", "Headphones");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->sink_combo), "bluez_output.AB");
  g_assert_true (wait_for_log (log, "set-default-sink bluez_output.AB", 5000));
  g_assert_true (wait_for_log (log, "move-sink-input 5 bluez_output.AB", 5000));
  g_assert_true (wait_for_log (log, "move-sink-input 9 bluez_output.AB", 5000));

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->source_combo),
                             "alsa_input.pci-0", "Built-in Mic");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->source_combo),
                               "alsa_input.pci-0");
  g_assert_true (wait_for_log (log, "set-default-source alsa_input.pci-0", 5000));
  g_assert_true (wait_for_log (log, "move-source-output", 5000));

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->sink_combo), "none", "None");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->sink_combo), "none");
  gtk_combo_box_set_active (GTK_COMBO_BOX (qs->sink_combo), -1);
  qs->updating = TRUE;
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->sink_combo),
                               "bluez_output.AB");
  qs->updating = FALSE;
  run_loop_ms (200);
  g_assert_false (log_contains (log, "set-default-sink none"));

  qs->audio_backend = AUDIO_NONE;
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->sink_combo), "noop", "Noop");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->sink_combo), "noop");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->source_combo),
                             "noop", "Noop");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->source_combo), "noop");
  run_loop_ms (200);
  g_assert_false (log_contains (log, "set-default-sink noop"));
  g_assert_false (log_contains (log, "set-default-source noop"));

  qs_unref (qs);
  g_remove (log);
  g_rmdir (dir);
  g_free (log);
  g_free (dir);
}

/* --- navegacao: tile abre pagina; voltar fecha --- */

static void
test_flow_navigation (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *main_box;
  GtkWidget     *tile;
  GtkWidget     *page;
  GtkWidget     *back;
  gchar         *dir;
  gchar         *log;
  const gchar   *visible;

  if (skip_no_display ())
    return;
  dir = make_fake_dir ();
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  qs = qs_new ();
  stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_NONE);
  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (stack), main_box, "main");
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  audio_build_section (box, stack, qs);
  qs->stack = stack;
  gtk_widget_show_all (stack);

  tile = find_button_with_label (box, "Audio devices");
  g_assert_nonnull (tile);
  g_signal_emit_by_name (tile, "clicked");
  run_loop_ms (500);
  visible = gtk_stack_get_visible_child_name (GTK_STACK (stack));
  g_assert_cmpstr (visible, ==, "audio");

  page = gtk_stack_get_child_by_name (GTK_STACK (stack), "audio");
  g_assert_nonnull (page);
  back = find_first_button (page);
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  visible = gtk_stack_get_visible_child_name (GTK_STACK (stack));
  g_assert_cmpstr (visible, ==, "main");

  qs_unref (qs);
  g_remove (log);
  g_rmdir (dir);
  g_free (log);
  g_free (dir);
}

/* --- fallback de widgets, valor invalido, clamp e qs morto --- */

static void
test_flow_done_fallbacks (void)
{
  QuickSettings *qs;
  gchar         *dir;
  gchar         *log;
  gchar         *old_path;
  gchar         *prefixed;
  gchar         *fake_path;

  if (skip_no_display ())
    return;
  dir = make_fake_dir ();
  write_fake_pactl (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  old_path = g_strdup (g_getenv ("PATH"));
  prefixed = g_strdup_printf ("%s:%s", dir, old_path != NULL ? old_path : "/bin");
  g_setenv ("PATH", prefixed, TRUE);
  g_free (prefixed);

  qs = qs_new ();
  qs->vol_scale = new_scale ();
  qs->vol_icon = gtk_image_new ();

  g_setenv ("QS_FAKE_VOLUME", "Volume: front-left: 131072 / 200% / 0 dB", TRUE);
  audio_refresh_volume (qs);
  g_assert_true (wait_for_range (GTK_RANGE (qs->vol_scale), 150, 5000));
  assert_tooltip (qs->vol_scale, "Volume (boosted above 100%)");

  g_setenv ("QS_FAKE_VOLUME", "Volume: muted", TRUE);
  audio_refresh_volume (qs);
  g_assert_true (wait_for_insensitive (qs->vol_scale, 5000));
  assert_tooltip (qs->vol_scale, "Volume unavailable");

  g_setenv ("QS_FAKE_VOLUME", "Volume: mono: 32768 / 50% / -18.06 dB", TRUE);
  audio_refresh_input_volume (qs);
  g_assert_true (wait_for_range (GTK_RANGE (qs->vol_scale), 50, 5000));

  g_setenv ("QS_FAKE_VOLUME", "Volume: muted", TRUE);
  audio_refresh_input_volume (qs);
  g_assert_true (wait_for_insensitive (qs->vol_scale, 5000));

  qs->dead = TRUE;
  audio_refresh_volume (qs);
  run_loop_ms (300);

  qs_unref (qs);
  g_unsetenv ("QS_FAKE_VOLUME");
  if (old_path != NULL)
    g_setenv ("PATH", old_path, TRUE);
  fake_path = g_build_filename (dir, "pactl", NULL);
  g_remove (fake_path);
  g_remove (log);
  g_rmdir (dir);
  g_free (fake_path);
  g_free (old_path);
  g_free (log);
  g_free (dir);
}

/* --- guarda de loading e idle apos dead --- */

static void
test_flow_devices_loading_and_dead (void)
{
  QuickSettings *qs;
  QuickSettings *dead_qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *popup;
  gchar         *dir;
  gchar         *log;

  if (skip_no_display ())
    return;
  dir = make_fake_dir ();
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

  qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);
  popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->popup = popup;
  g_object_set_data (G_OBJECT (popup), "qs-audio-loading", GUINT_TO_POINTER (1));
  audio_refresh_devices (qs);
  g_assert_null (gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->sink_combo)));

  g_object_set_data (G_OBJECT (popup), "qs-audio-loading", NULL);
  audio_refresh_devices (qs);
  g_assert_true (wait_for_combo_id (GTK_COMBO_BOX (qs->sink_combo),
                                    "alsa_output.pci-0", 5000));
  g_assert_null (g_object_get_data (G_OBJECT (popup), "qs-audio-loading"));

  gtk_widget_destroy (popup);
  qs->popup = NULL;
  qs_unref (qs);

  dead_qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, dead_qs);
  dead_qs->dead = TRUE;
  audio_refresh_devices (dead_qs);
  run_loop_ms (500);
  qs_unref (dead_qs);

  g_remove (log);
  g_rmdir (dir);
  g_free (log);
  g_free (dir);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gchar *fakebin;
  gchar *old_path;

  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);

  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  old_path = g_strdup (g_getenv ("PATH"));
  {
    gchar *prefixed = g_strdup_printf ("%s:%s", fakebin,
                                       old_path != NULL ? old_path : "/bin");

    g_setenv ("PATH", prefixed, TRUE);
    g_free (prefixed);
  }
  g_free (old_path);
  g_free (fakebin);
  g_free (cwd);

  g_test_add_func ("/audio-flow/refresh", test_flow_refresh);
  g_test_add_func ("/audio-flow/device_changes", test_flow_device_changes);
  g_test_add_func ("/audio-flow/navigation", test_flow_navigation);
  g_test_add_func ("/audio-flow/done_fallbacks", test_flow_done_fallbacks);
  g_test_add_func ("/audio-flow/devices_loading_and_dead",
                   test_flow_devices_loading_and_dead);
  return g_test_run ();
}
