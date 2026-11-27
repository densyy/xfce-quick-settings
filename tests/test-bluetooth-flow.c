/* test-bluetooth-flow.c — fluxos da página Bluetooth com fakebin.
 *
 * Dirige qs-bluetooth.c pela UI real (builders + sinais) e por cache/tile,
 * sempre com PATH prefixado por tests/fakebin e QS_FAKE_LOG provando os
 * comandos. Nenhum bluetoothctl real é executado. Sem display, pula.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-net.h"
#include "qs-util.h"
#include "qs-bt-backend.h"

static gboolean have_display = FALSE;
static gchar   *fakebin = NULL;
static gchar   *empty_bin = NULL;
static gchar   *base_path = NULL;
static gchar   *tmp_root = NULL;

typedef gboolean (*WaitFn) (gpointer data);

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
wait_until (WaitFn      fn,
            gpointer    data,
            guint       timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (fn (data))
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return fn (data);
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
reset_fake_env (void)
{
  static const gchar *vars[] = {
    "QS_FAKE_BT_NO_ADAPTER", "QS_FAKE_BT_POWER",
    "QS_FAKE_BT_EMPTY", "QS_FAKE_BT_DEVICES",
    "QS_FAKE_BT_PAIRED", "QS_FAKE_BT_TRUSTED", "QS_FAKE_BT_CONNECTED",
    "QS_FAKE_BT_INFO_FAIL", "QS_FAKE_BT_INFO_EMPTY",
    "QS_FAKE_BT_INFO_PAIRED", "QS_FAKE_BT_INFO_TRUSTED",
    "QS_FAKE_BT_INFO_CONNECTED", "QS_FAKE_BT_STATE",
    "QS_FAKE_BT_CONNECT_FAIL", "QS_FAKE_BT_DISCONNECT_FAIL",
    "QS_FAKE_BT_CONNECT_DELAY_MS", "QS_FAKE_BT_CONNECT_STATE_EARLY",
    "QS_FAKE_BT_KEEP_STATE", "QS_BT_LINK_TIMEOUT_SECS"
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (vars); i++)
    g_unsetenv (vars[i]);
}

static void
set_path_prefixed (void)
{
  g_setenv ("PATH", base_path, TRUE);
}

static void
set_path_without_bt (void)
{
  g_setenv ("PATH", empty_bin, TRUE);
}

static gchar *
new_log (void)
{
  gchar *dir = g_dir_make_tmp ("qs-bt-flow-log-XXXXXX", NULL);
  gchar *log;

  g_assert_nonnull (dir);
  log = g_build_filename (dir, "argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_remove (log);
  g_free (dir);
  return log;
}

static gchar *
read_log (const gchar *path)
{
  gchar *text = NULL;

  g_file_get_contents (path, &text, NULL, NULL);
  return text;
}

static gboolean
log_has (const gchar *path,
         const gchar *needle)
{
  gchar   *text = read_log (path);
  gboolean hit = (text != NULL && strstr (text, needle) != NULL);

  g_free (text);
  return hit;
}

static guint
log_count (const gchar *path,
           const gchar *needle)
{
  gchar       *text = read_log (path);
  const gchar *p;
  guint        n = 0;

  if (text == NULL)
    return 0;
  p = text;
  while ((p = strstr (p, needle)) != NULL)
    {
      n++;
      p += strlen (needle);
    }
  g_free (text);
  return n;
}

typedef struct
{
  const gchar *path;
  const gchar *needle;
} LogWait;

static gboolean
log_wait_ready (gpointer data)
{
  LogWait *w = data;

  return log_has (w->path, w->needle);
}

static gboolean
wait_log_struct (const gchar *path,
                 const gchar *needle,
                 guint        timeout_ms)
{
  LogWait w = { path, needle };

  return wait_until (log_wait_ready, &w, timeout_ms);
}

typedef struct
{
  GtkWidget   *label;
  const gchar *text;
  gboolean     contains;
} LabelWait;

static gboolean
label_wait_ready (gpointer data)
{
  LabelWait  *w = data;
  const gchar *text = gtk_label_get_text (GTK_LABEL (w->label));

  if (w->contains)
    return (text != NULL && strstr (text, w->text) != NULL);
  return g_strcmp0 (text, w->text) == 0;
}

static gboolean
wait_label (GtkWidget   *label,
            const gchar *text,
            gboolean     contains,
            guint        timeout_ms)
{
  LabelWait w = { label, text, contains };

  return wait_until (label_wait_ready, &w, timeout_ms);
}

static gboolean
load_settled (gpointer data)
{
  QuickSettings *qs = data;

  return (!qs->scanning_bt && gtk_widget_get_sensitive (qs->bt_scan));
}

static gboolean
wait_load_settled (QuickSettings *qs,
                   guint          timeout_ms)
{
  return wait_until (load_settled, qs, timeout_ms);
}

static gboolean
probe_settled (gpointer data)
{
  QuickSettings *qs = data;

  return !qs->bt_tile_probe;
}

static gboolean
wait_probe_settled (QuickSettings *qs,
                    guint          timeout_ms)
{
  return wait_until (probe_settled, qs, timeout_ms);
}

static GtkWidget *
make_fixture (QuickSettings *qs,
              GtkWidget   **stack_out)
{
  GtkWidget *stack = gtk_stack_new ();
  GtkWidget *main_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *back = NULL;

  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named (GTK_STACK (stack), main_page, "main");
  bt_build_page (stack, qs, &back);
  qs->stack = stack;
  qs->bt_tile = gtk_button_new ();
  qs->bt_sub = gtk_label_new ("…");
  g_object_ref_sink (qs->bt_tile);
  g_object_ref_sink (qs->bt_sub);
  gtk_widget_show_all (stack);
  *stack_out = stack;
  return stack;
}

static void
free_fixture (QuickSettings *qs,
              GtkWidget     *stack,
              const gchar   *log)
{
  bt_cancel_all ();
  run_loop_ms (30);
  if (stack != NULL)
    gtk_widget_destroy (stack);
  if (qs->bt_tile != NULL)
    {
      gtk_widget_destroy (qs->bt_tile);
      g_object_unref (qs->bt_tile);
      qs->bt_tile = NULL;
    }
  if (qs->bt_sub != NULL)
    {
      gtk_widget_destroy (qs->bt_sub);
      g_object_unref (qs->bt_sub);
      qs->bt_sub = NULL;
    }
  if (qs->popup != NULL)
    {
      gtk_widget_hide (qs->popup);
      gtk_widget_destroy (qs->popup);
      qs->popup = NULL;
    }
  bt_cancel_all ();
  bt_free_caches ();
  qs_unref (qs);
  if (log != NULL)
    g_remove (log);
  g_free ((gpointer) log);
}

static guint
bt_row_count (QuickSettings *qs)
{
  GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->bt_list));
  GList *l;
  guint  n = 0;

  for (l = rows; l != NULL; l = l->next)
    if (GTK_IS_LIST_BOX_ROW (l->data))
      n++;
  g_list_free (rows);
  return n;
}

static GtkWidget *
find_row (QuickSettings *qs,
          const gchar   *mac)
{
  GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->bt_list));
  GList *l;
  GtkWidget *hit = NULL;

  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *m;

      if (!GTK_IS_LIST_BOX_ROW (l->data))
        continue;
      m = g_object_get_data (G_OBJECT (l->data), "qs-mac");
      if (g_strcmp0 (m, mac) == 0)
        {
          hit = GTK_WIDGET (l->data);
          break;
        }
    }
  g_list_free (rows);
  return hit;
}

static gboolean
row_connected (GtkWidget *row)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "qs-connected"));
}

static gboolean
row_paired (GtkWidget *row)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "qs-paired"));
}

static void
assert_file_text (const gchar *path,
                  const gchar *expected)
{
  gchar *text = read_log (path);

  g_assert_cmpstr (text, ==, expected);
  g_free (text);
}

static const gchar *
row_state (GtkWidget *row)
{
  GtkWidget *hbox = gtk_bin_get_child (GTK_BIN (row));
  GList     *kids;
  GList     *l;
  gint       labels = 0;
  const gchar *state = "";

  if (hbox == NULL || !GTK_IS_BOX (hbox))
    return "";
  kids = gtk_container_get_children (GTK_CONTAINER (hbox));
  for (l = kids; l != NULL; l = l->next)
    {
      if (GTK_IS_LABEL (l->data))
        {
          labels++;
          if (labels == 2)
            state = gtk_label_get_text (GTK_LABEL (l->data));
        }
    }
  g_list_free (kids);
  return state;
}

static GtkWidget *
make_plain_row (QuickSettings *qs,
                const gchar   *mac,
                const gchar   *name)
{
  GtkWidget *row = gtk_list_box_row_new ();

  if (mac != NULL)
    g_object_set_data_full (G_OBJECT (row), "qs-mac", g_strdup (mac), g_free);
  if (name != NULL)
    g_object_set_data_full (G_OBJECT (row), "qs-name", g_strdup (name), g_free);
  gtk_container_add (GTK_CONTAINER (qs->bt_list), row);
  gtk_widget_show_all (row);
  return row;
}

static void
create_state_file (const gchar *path,
                   const gchar *value)
{
  gchar *content = g_strdup_printf ("%s\n", value);

  g_file_set_contents (path, content, -1, NULL);
  g_free (content);
}

/* --- página, carga e cache --- */

static void
test_page_load_and_cache (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  bt_show_page (qs);
  g_assert_true (qs->scanning_bt);
  g_assert_false (gtk_widget_is_sensitive (qs->bt_scan));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==, "Loading…");
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==, "");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Headset");
  g_assert_cmpuint (bt_row_count (qs), ==, 3);
  g_assert_cmpstr (qs->bt_controller, ==, "AA:BB:CC:DD:EE:FF");
  g_assert_true (qs->bt_on);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (qs->bt_switch)));

  row = find_row (qs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Connected");
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_paired (row));
  g_assert_cmpstr (row_state (row), ==, "Paired");
  row = find_row (qs, "AA:AA:AA:AA:AA:01");
  g_assert_nonnull (row);
  g_assert_cmpstr (row_state (row), ==, "");

  bt_show_page (qs);
  g_assert_false (qs->scanning_bt);
  g_assert_cmpuint (bt_row_count (qs), ==, 3);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Headset");

  bt_update_tile_cached (qs);
  g_assert_true (gtk_widget_is_sensitive (qs->bt_tile));
  g_assert_true (gtk_style_context_has_class (
    gtk_widget_get_style_context (qs->bt_tile), "qs-tile-active"));

  qs->scanning_bt = TRUE;
  bt_show_page (qs);
  g_assert_true (qs->scanning_bt);
  qs->scanning_bt = FALSE;

  bt_free_caches ();
  bt_show_page (qs);
  g_assert_true (qs->scanning_bt);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 3);

  free_fixture (qs, stack, log);
}

static void
test_page_off_and_empty (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_setenv ("QS_FAKE_BT_POWER", "no", TRUE);
  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Bluetooth is off.");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Off");
  g_assert_true (gtk_widget_is_sensitive (qs->bt_switch));
  g_assert_false (qs->bt_on);
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  bt_show_page (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Bluetooth is off.");
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  g_unsetenv ("QS_FAKE_BT_POWER");
  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_setenv ("QS_FAKE_BT_EMPTY", "1", TRUE);
  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==, "");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "…");
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  bt_show_page (qs);
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  free_fixture (qs, stack, log);
}

static void
test_page_no_adapter_and_missing_tool (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_setenv ("QS_FAKE_BT_NO_ADAPTER", "1", TRUE);
  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "No Bluetooth adapter found.");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==,
                   "No adapter");
  g_assert_false (gtk_widget_is_sensitive (qs->bt_switch));
  g_assert_null (qs->bt_controller);
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  bt_show_page (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==,
                   "No adapter");
  g_assert_cmpuint (bt_row_count (qs), ==, 1);

  gtk_stack_set_visible_child_name (GTK_STACK (stack), "main");
  set_path_without_bt ();
  bt_show_page (qs);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (stack)), ==,
                   "main");

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  set_path_without_bt ();
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*run_command failed*");
  g_assert_true (wait_label (qs->bt_status, "Install bluez-utils.", FALSE,
                             4000));
  g_test_assert_expected_messages ();
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==,
                   "No adapter");
  g_assert_false (gtk_widget_is_sensitive (qs->bt_switch));
  g_assert_true (wait_load_settled (qs, 3000));

  set_path_prefixed ();
  free_fixture (qs, stack, log);
}

/* --- scan --- */

static void
test_scan_flow (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;
  guint          before;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_button_clicked (GTK_BUTTON (qs->bt_scan));
  g_assert_true (qs->scanning_bt);
  g_assert_false (gtk_widget_is_sensitive (qs->bt_scan));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Scanning for devices…");
  g_assert_true (wait_log_struct (log, "scan on", 3000));
  g_assert_true (wait_load_settled (qs, 6000));
  g_assert_true (log_has (log, "devices"));
  g_assert_cmpuint (bt_row_count (qs), ==, 3);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==, "");
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (qs->bt_scan)), ==,
                   "Scan for devices");

  before = log_count (log, "scan on");
  qs->scanning_bt = TRUE;
  gtk_button_clicked (GTK_BUTTON (qs->bt_scan));
  run_loop_ms (100);
  g_assert_cmpuint (log_count (log, "scan on"), ==, before);
  qs->scanning_bt = FALSE;

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_button_clicked (GTK_BUTTON (qs->bt_scan));
  qs->dead = TRUE;
  run_loop_ms (500);
  g_assert_true (qs->dead);
  g_assert_false (log_has (log, "devices"));

  free_fixture (qs, stack, log);
}

/* --- switch e reload --- */

static void
test_switch_reload (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  g_assert_true (qs->bt_on);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Turning Bluetooth on…");
  g_assert_true (wait_log_struct (log, "power on", 2000));
  g_assert_true (wait_label (qs->bt_status, "", FALSE, 5000));
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 3);

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  qs->scanning_bt = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  run_loop_ms (1300);
  g_assert_true (qs->scanning_bt);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Turning Bluetooth on…");
  qs->scanning_bt = FALSE;

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  qs->dead = TRUE;
  run_loop_ms (1300);
  g_assert_true (qs->dead);

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  qs->updating = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), TRUE);
  run_loop_ms (100);
  g_assert_false (qs->bt_on);
  g_assert_false (log_has (log, "power on"));
  qs->updating = FALSE;

  free_fixture (qs, stack, log);
}

/* --- tile assíncrono --- */

static void
test_tile_probe (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  bt_refresh_tile_async (qs);
  g_assert_true (qs->bt_tile_probe);
  g_assert_true (wait_probe_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Headset");
  g_assert_true (qs->bt_on);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (qs->bt_switch)));
  g_assert_true (gtk_style_context_has_class (
    gtk_widget_get_style_context (qs->bt_tile), "qs-tile-active"));

  qs->bt_tile_probe = TRUE;
  bt_refresh_tile_async (qs);
  qs->bt_tile_probe = FALSE;

  qs->scanning_bt = TRUE;
  bt_refresh_tile_async (qs);
  g_assert_false (qs->bt_tile_probe);
  qs->scanning_bt = FALSE;

  set_path_without_bt ();
  bt_refresh_tile_async (qs);
  g_assert_false (qs->bt_tile_probe);
  set_path_prefixed ();

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_setenv ("QS_FAKE_BT_POWER", "no", TRUE);
  bt_refresh_tile_async (qs);
  g_assert_true (wait_probe_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Off");
  g_assert_false (qs->bt_on);

  g_unsetenv ("QS_FAKE_BT_POWER");
  g_setenv ("QS_FAKE_BT_EMPTY", "1", TRUE);
  bt_refresh_tile_async (qs);
  g_assert_true (wait_probe_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "On");
  g_assert_true (qs->bt_on);

  g_unsetenv ("QS_FAKE_BT_EMPTY");
  g_setenv ("QS_FAKE_BT_NO_ADAPTER", "1", TRUE);
  bt_refresh_tile_async (qs);
  g_assert_true (wait_probe_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==,
                   "No adapter");

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  gtk_label_set_text (GTK_LABEL (qs->bt_sub), "keep");
  bt_refresh_tile_async (qs);
  qs->scanning_bt = TRUE;
  g_assert_true (wait_probe_settled (qs, 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "keep");
  qs->scanning_bt = FALSE;

  bt_refresh_tile_async (qs);
  qs->dead = TRUE;
  g_assert_true (wait_probe_settled (qs, 5000));

  free_fixture (qs, stack, log);
}

/* --- link/detalhe --- */

static void
enable_connect_success (void)
{
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
}

static void
test_link_connect_success (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "On");

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Reading device…");
  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 2);
  g_assert_true (log_has (log, "connect 11:22:33:44:55:66"));
  assert_file_text (state, "yes\n");
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Connected");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");

  bt_show_page (qs);
  g_assert_false (qs->scanning_bt);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_pair_trust_connect (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-pair", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "no", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "no", TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Reading device…");
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_true (log_has (log, "pair 11:22:33:44:55:66"));
  g_assert_true (log_has (log, "trust 11:22:33:44:55:66"));
  g_assert_true (log_has (log, "connect 11:22:33:44:55:66"));
  g_assert_true (log_has (log, "info 11:22:33:44:55:66"));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 2);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_disconnect_success (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-off", NULL);
  create_state_file (state, "yes");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device BB:BB:BB:BB:BB:02 Headset", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECTED", "Device BB:BB:BB:BB:BB:02 Headset", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Headset");

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Reading device…");
  g_assert_true (wait_label (qs->bt_status, "Disconnected from Headset.", FALSE,
                             5000));
  g_assert_true (log_has (log, "disconnect BB:BB:BB:BB:BB:02"));
  assert_file_text (state, "no\n");
  row = find_row (qs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Paired");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "On");

  g_free (state);
  free_fixture (qs, stack, log);
}

/* Regressão do bug reportado: o `connect` é cortado pelo watchdog (o
 * timeout genérico de 5 s matava o bluetoothctl no meio da conexão),
 * mas o BlueZ conecta o aparelho mesmo assim. A UI não pode mostrar
 * "Failed to connect": o estado real do dispositivo é o que manda. */
static void
test_link_connect_cut_short_but_connected (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-cut", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  /* Watchdog curto: o connect é cortado no meio. */
  g_setenv ("QS_BT_LINK_TIMEOUT_SECS", "1", TRUE);
  /* O connect demora 6 s (mais que o watchdog genérico de 5 s), mas o
   * BlueZ conecta logo no começo. */
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "6000", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_STATE_EARLY", "1", TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 10000));
  assert_file_text (state, "yes\n");
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));

  g_free (state);
  free_fixture (qs, stack, log);
}

/* O `connect` pode bloquear pelo timeout de página do BlueZ (10 s+): o
 * comando precisa de um watchdog maior que o genérico de 5 s, senão ele
 * morre antes de responder. */
static void
test_link_connect_slow_but_ok (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-slow", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  /* Conecta depois de 6 s: maior que o watchdog genérico de 5 s. */
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "6000", TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 15000));
  assert_file_text (state, "yes\n");

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_failure_detail (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-fail", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_FAIL", "2", TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  /* Antes de acusar falha o estado real é conferido (o comando pode ter
   * sido cortado com o BlueZ ainda trabalhando). */
  g_assert_true (wait_label (qs->bt_status,
                             "Failed to connect Buds: Failed to connect: "
                             "org.bluez.Error.Failed", FALSE, 15000));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), >=, 2);
  assert_file_text (state, "no\n");

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_toggle_info_unavailable (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  row = make_plain_row (qs, "11:22:33:44:55:66", "Buds");
  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  set_path_without_bt ();
  g_assert_true (wait_label (qs->bt_status,
                             "Failed to read Buds: could not query device info",
                             FALSE, 3000));
  set_path_prefixed ();

  free_fixture (qs, stack, log);
}

static void
test_link_guards (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_signal_emit_by_name (qs->bt_list, "row-selected", NULL);

  row = make_plain_row (qs, "11:22:33:44:55:66", "Buds");
  gtk_widget_set_sensitive (row, FALSE);
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);

  row = make_plain_row (qs, NULL, "NoMac");
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);

  run_loop_ms (100);
  g_assert_cmpuint (log_count (log, "info"), ==, 0);

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  row = make_plain_row (qs, "11:22:33:44:55:66", "Buds");
  qs->dead = TRUE;
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (log, "info"), ==, 0);

  free_fixture (qs, stack, log);
}

static void
test_link_cancel (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  row = make_plain_row (qs, "11:22:33:44:55:66", "Buds");
  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  bt_cancel_all ();
  bt_cancel_all ();
  run_loop_ms (500);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Reading device…");
  g_assert_cmpuint (log_count (log, "connect"), ==, 0);

  free_fixture (qs, stack, log);
}

static void
test_link_row_missing_fallback (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-row", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  gtk_container_remove (GTK_CONTAINER (qs->bt_list), row);
  g_assert_cmpuint (bt_row_count (qs), ==, 0);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 1);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_verify_retry_failure (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  guint          waited = 0;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-retry", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_KEEP_STATE", "1", TRUE);
  enable_connect_success ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  while (waited < 10000
         && log_count (log, "info 11:22:33:44:55:66") < 9)
    {
      run_loop_ms (20);
      waited += 20;
    }
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), >=, 9);
  assert_file_text (state, "no\n");
  run_loop_ms (500);

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_verify_dead (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-dead", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_KEEP_STATE", "1", TRUE);
  enable_connect_success ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "info 11:22:33:44:55:66", 3000));
  run_loop_ms (900);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Verifying…");
  qs->dead = TRUE;
  run_loop_ms (1200);
  g_assert_true (qs->dead);

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_verify_fast_refresh (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  gint64         flipped_us;
  gint64         done_us;
  guint          latency_ms;
  guint          waited = 0;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-fast", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_KEEP_STATE", "1", TRUE);
  enable_connect_success ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));

  /* O BlueZ só vira Connected=yes DEPOIS que o `connect` retornou: o
   * verify precisa re-ler `info` até ver o estado novo. O estado vira
   * `yes` logo após o verify fazer a primeira releitura (2º `info` no
   * log, o 1º é o do toggle), então a latência medida é puro intervalo
   * de poll: ~250 ms na cadência nova, ~800 ms na antiga. A virada
   * espera 150 ms para não ganhar do `info` em voo (que leria o estado
   * novo no mesmo poll em qualquer cadência e esconderia a regressão). */
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "info 11:22:33:44:55:66", 3000));
  while (waited < 5000
         && log_count (log, "info 11:22:33:44:55:66") < 2)
    {
      run_loop_ms (20);
      waited += 20;
    }
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), >=, 2);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Verifying…");
  /* Dá tempo do `info` em voo ler o estado velho antes da virada. */
  run_loop_ms (150);
  g_file_set_contents (state, "yes\n", -1, NULL);
  flipped_us = g_get_monotonic_time ();
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 8000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - flipped_us) / 1000);
  /* Cadência nova: próximo poll em ~250 ms; antiga: ~800 ms. */
  g_assert_cmpuint (latency_ms, <, 450);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), >=, 3);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");

  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_hide_nameless_devices (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  g_setenv ("QS_FAKE_BT_DEVICES",
            "Device AA:BB:CC:DD:EE:11\n"
            "Device AA:BB:CC:DD:EE:12 AA:BB:CC:DD:EE:12\n"
            "Device 11:22:33:44:55:66 Buds",
            TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 1);
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:11"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:12"));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  free_fixture (qs, stack, log);
}

static void
test_teardown_idempotent (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  gchar         *log;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_widget_hide (qs->popup);

  bt_free_caches ();
  bt_cancel_all ();
  bt_free_caches ();
  bt_cancel_all ();

  free_fixture (qs, stack, log);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gchar *prefixed;

  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-bt-flow-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);

  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  g_assert_true (g_file_test (fakebin, G_FILE_TEST_IS_DIR));
  g_free (cwd);
  empty_bin = g_dir_make_tmp ("qs-bt-flow-empty-XXXXXX", NULL);
  g_assert_nonnull (empty_bin);
  prefixed = g_strdup_printf ("%s:%s", fakebin, g_getenv ("PATH"));
  base_path = prefixed;

  g_test_add_func ("/bt-flow/page_load_and_cache", test_page_load_and_cache);
  g_test_add_func ("/bt-flow/page_off_and_empty", test_page_off_and_empty);
  g_test_add_func ("/bt-flow/page_no_adapter", test_page_no_adapter_and_missing_tool);
  g_test_add_func ("/bt-flow/scan", test_scan_flow);
  g_test_add_func ("/bt-flow/switch_reload", test_switch_reload);
  g_test_add_func ("/bt-flow/tile_probe", test_tile_probe);
  g_test_add_func ("/bt-flow/link_connect_success", test_link_connect_success);
  g_test_add_func ("/bt-flow/link_connect_cut_short",
                   test_link_connect_cut_short_but_connected);
  g_test_add_func ("/bt-flow/link_connect_slow",
                   test_link_connect_slow_but_ok);
  g_test_add_func ("/bt-flow/link_pair_trust", test_link_pair_trust_connect);
  g_test_add_func ("/bt-flow/link_disconnect", test_link_disconnect_success);
  g_test_add_func ("/bt-flow/link_failure_detail", test_link_failure_detail);
  g_test_add_func ("/bt-flow/link_info_unavailable", test_link_toggle_info_unavailable);
  g_test_add_func ("/bt-flow/link_guards", test_link_guards);
  g_test_add_func ("/bt-flow/link_cancel", test_link_cancel);
  g_test_add_func ("/bt-flow/link_row_fallback", test_link_row_missing_fallback);
  g_test_add_func ("/bt-flow/link_verify_retry", test_link_verify_retry_failure);
  g_test_add_func ("/bt-flow/link_verify_dead", test_link_verify_dead);
  g_test_add_func ("/bt-flow/link_verify_fast_refresh",
                   test_link_verify_fast_refresh);
  g_test_add_func ("/bt-flow/hide_nameless", test_hide_nameless_devices);
  g_test_add_func ("/bt-flow/teardown", test_teardown_idempotent);

  return g_test_run ();
}
