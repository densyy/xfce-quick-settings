/* test-net.c — cobertura da fachada Connection (qs-net.c).
 *
 * Constrói a seção completa (grupo "Connection", tiles Wi-Fi/Bluetooth,
 * páginas "wifi"/"bt" e botões voltar), clica nos tiles (navegação +
 * load async) e nos botões qs-back (volta para "main" + net_update_tiles),
 * exercita net_refresh_bt_tile e net_cleanup (idempotente).
 *
 * PATH contém SOMENTE tests/fakebin: nmcli/bluetoothctl falsos; nenhuma
 * ferramenta real é executada (QS_FAKE_LOG prova os argv chamados).
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-net.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;

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

static gchar *
log_read (const gchar *path)
{
  gchar *text = NULL;

  g_file_get_contents (path, &text, NULL, NULL);
  return (text != NULL) ? text : g_strdup ("");
}

static gboolean
log_has (const gchar *path,
         const gchar *needle)
{
  gchar   *text = log_read (path);
  gboolean hit = (strstr (text, needle) != NULL);

  g_free (text);
  return hit;
}

static guint
log_count (const gchar *path,
           const gchar *needle)
{
  gchar       *text = log_read (path);
  const gchar *p = text;
  guint        n = 0;

  while ((p = strstr (p, needle)) != NULL)
    {
      n++;
      p++;
    }
  g_free (text);
  return n;
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

static gboolean
wait_log_count (const gchar *path,
                const gchar *needle,
                guint        want,
                guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_count (path, needle) >= want)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return log_count (path, needle) >= want;
}

/* Espera os loads async de página (Wi-Fi/Bluetooth) terminarem. */
static gboolean
wait_pages_idle (const QuickSettings *qs,
                 guint                timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (!qs->scanning_wifi && !qs->scanning_bt)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return !qs->scanning_wifi && !qs->scanning_bt;
}

static gboolean
wait_tile_probe_done (const QuickSettings *qs,
                      guint                timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (!qs->bt_tile_probe)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return !qs->bt_tile_probe;
}

static gboolean
tree_has_label (GtkWidget   *root,
                const gchar *text)
{
  if (GTK_IS_LABEL (root)
      && g_strcmp0 (gtk_label_get_text (GTK_LABEL (root)), text) == 0)
    return TRUE;
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      gboolean   hit = FALSE;

      for (l = kids; l != NULL && !hit; l = l->next)
        hit = tree_has_label (GTK_WIDGET (l->data), text);
      g_list_free (kids);
      return hit;
    }
  return FALSE;
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

/* --- estrutura --- */

static void
test_net_build_section (void)
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
  GtkWidget     *wifi_page;
  GtkWidget     *bt_page;

  if (skip_no_display ())
    return;

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

  net_build_section (box, stack, qs);
  gtk_widget_show_all (box);
  gtk_widget_show_all (stack);

  g_assert_true (tree_has_label (box, "Connection"));
  g_assert_true (GTK_IS_BUTTON (qs->wifi_tile));
  g_assert_true (GTK_IS_BUTTON (qs->bt_tile));
  g_assert_true (tree_has_label (qs->wifi_tile, "Wi-Fi"));
  g_assert_true (tree_has_label (qs->bt_tile, "Bluetooth"));
  g_assert_nonnull (qs->wifi_sub);
  g_assert_nonnull (qs->bt_sub);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->wifi_sub)), ==, "…");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "…");

  wifi_page = gtk_stack_get_child_by_name (GTK_STACK (stack), "wifi");
  bt_page = gtk_stack_get_child_by_name (GTK_STACK (stack), "bt");
  g_assert_nonnull (wifi_page);
  g_assert_nonnull (bt_page);
  g_assert_nonnull (find_back_button (wifi_page));
  g_assert_nonnull (find_back_button (bt_page));

  gtk_widget_destroy (box);
  gtk_widget_destroy (stack);
  gtk_widget_destroy (popup);
  qs_unref (qs);
}

/* --- navegação + loads async + update/refresh + cleanup --- */

static void
test_net_navigation_flow (void)
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
  GtkWidget     *page;
  GtkWidget     *back;
  gchar         *log;
  guint          radio0;
  guint          paired0;

  if (skip_no_display ())
    return;

  log = g_build_filename (tmp_root, "test-net-flow.log", NULL);
  g_remove (log);
  g_setenv ("QS_FAKE_LOG", log, TRUE);

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
  net_build_section (box, stack, qs);
  gtk_widget_show_all (box);
  gtk_widget_show_all (stack);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "main");

  /* Wi-Fi: tile -> página "wifi" + load async (nmcli falso). */
  g_signal_emit_by_name (qs->wifi_tile, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)),
                   ==, "wifi");
  g_assert_true (wait_log (log, "device wifi list", 3000));
  g_assert_true (wait_log (log, "radio wifi", 3000));
  g_assert_true (wait_pages_idle (qs, 3000));
  g_assert_false (qs->scanning_wifi);

  /* Voltar: net_update_tiles (cache + probe async) + "main". */
  radio0 = log_count (log, "radio wifi");
  page = gtk_stack_get_child_by_name (GTK_STACK (stack), "wifi");
  back = find_back_button (page);
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)),
                   ==, "main");
  g_assert_cmpuint (qs->cache_seq, >=, 1);
  g_assert_true (wait_log_count (log, "radio wifi", radio0 + 1, 3000));
  run_loop_ms (200);

  /* Bluetooth: tile -> página "bt" + load async (bluetoothctl falso). */
  g_signal_emit_by_name (qs->bt_tile, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)),
                   ==, "bt");
  g_assert_true (wait_log (log, "show AA:BB:CC:DD:EE:FF", 3000));
  g_assert_true (wait_log (log, "devices Paired", 3000));
  g_assert_true (wait_pages_idle (qs, 3000));
  g_assert_false (qs->scanning_bt);

  /* Voltar pela página BT: também passa por net_update_tiles. */
  page = gtk_stack_get_child_by_name (GTK_STACK (stack), "bt");
  back = find_back_button (page);
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)),
                   ==, "main");
  run_loop_ms (200);

  /* net_refresh_bt_tile: probe assíncrono do tile. */
  paired0 = log_count (log, "devices Paired");
  net_refresh_bt_tile (qs);
  g_assert_true (qs->bt_tile_probe);
  g_assert_true (wait_tile_probe_done (qs, 3000));
  g_assert_true (wait_log_count (log, "devices Paired", paired0 + 1, 3000));
  run_loop_ms (200);

  /* net_cleanup é idempotente. */
  net_cleanup ();
  net_cleanup ();
  net_cleanup ();

  run_loop_ms (300);
  gtk_widget_destroy (box);
  gtk_widget_destroy (stack);
  gtk_widget_destroy (popup);
  qs_unref (qs);
  g_unsetenv ("QS_FAKE_LOG");
  g_free (log);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-net-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);

  /* PATH só com fakes: nmcli/bluetoothctl reais nunca são chamados. */
  fakebin = g_build_filename (g_get_current_dir (), "tests", "fakebin", NULL);
  g_assert_true (g_file_test (fakebin, G_FILE_TEST_IS_DIR));
  g_setenv ("PATH", fakebin, TRUE);
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");

  g_test_add_func ("/net/build_section", test_net_build_section);
  g_test_add_func ("/net/navigation_flow", test_net_navigation_flow);

  return g_test_run ();
}
