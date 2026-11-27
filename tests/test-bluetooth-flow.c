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
#include "qs-bt-agent.h"
#include "qs-bt-backend.h"
#include "qs-bt-signal.h"
#include "bt-signal-helper.h"

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
    "QS_FAKE_BT_KEEP_STATE", "QS_BT_LINK_TIMEOUT_SECS",
    "QS_BT_SIGNAL_TIMEOUT_SECS",
    "QS_FAKE_BT_PAIR_FAIL", "QS_FAKE_BT_TRUST_FAIL",
    "QS_FAKE_BT_ALIAS_FAIL",
    "QS_FAKE_BT_SIGNAL", "QS_FAKE_BT_SIGNAL_PATH",
    "QS_FAKE_BT_SIGNAL_SENDER"
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (vars); i++)
    g_unsetenv (vars[i]);
  bt_signal_set_subscribe_fn (NULL);
}

static BtSignalWatch *captured_watch = NULL;
static BtSignalWatch *captured_page_watch = NULL;
static BtSignalWatch *captured_link_watch = NULL;
static guint subscribe_count = 0;

static BtSignalWatch *
test_subscribe_capture (BtSignalChangedCb cb,
                        gpointer          user_data,
                        GError          **error)
{
  BtSignalWatch *watch;

  (void) error;
  watch = bt_signal_watch_new_for_test (cb, user_data);
  if (subscribe_count == 0)
    captured_page_watch = watch;
  else
    captured_link_watch = watch;
  captured_watch = watch;
  subscribe_count++;
  return watch;
}

static void
test_subscribe_install (void)
{
  captured_watch = NULL;
  captured_page_watch = NULL;
  captured_link_watch = NULL;
  subscribe_count = 0;
  bt_signal_set_subscribe_fn (test_subscribe_capture);
}

static void
test_subscribe_restore (void)
{
  bt_signal_set_subscribe_fn (NULL);
  captured_watch = NULL;
  captured_page_watch = NULL;
  captured_link_watch = NULL;
  subscribe_count = 0;
}

static void
test_emit_connected (const gchar *mac,
                     gboolean     connected)
{
  g_assert_nonnull (captured_watch);
  bt_helper_emit_connected (captured_watch, mac, connected);
}

static void
test_emit_without_connected (const gchar *mac)
{
  g_assert_nonnull (captured_watch);
  bt_helper_emit_without_connected (captured_watch, mac);
}

static void
test_emit_page_properties (const gchar *mac,
                           gboolean     paired,
                           gboolean     connected)
{
  g_assert_nonnull (captured_page_watch);
  bt_helper_emit_page_properties (captured_page_watch, mac, paired,
                                  connected);
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
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

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
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2000", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "On");

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  g_assert_cmpint (subscribe_count, ==, 2);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  emit_us = g_get_monotonic_time ();
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 500);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  g_assert_true (log_has (log, "connect 11:22:33:44:55:66"));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Connected");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");
  run_loop_ms (2600);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 1000));

  test_subscribe_restore ();
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
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "1500", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 8000));
  g_assert_cmpint (subscribe_count, ==, 2);
  g_assert_true (log_has (log, "pair 11:22:33:44:55:66"));
  g_assert_true (log_has (log, "trust 11:22:33:44:55:66"));
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));

  test_subscribe_restore ();
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
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

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
  g_setenv ("QS_FAKE_BT_PAIRED", "Device BB:BB:BB:BB:BB:02 Headset", TRUE);
  g_setenv ("QS_FAKE_BT_TRUSTED", "Device BB:BB:BB:BB:BB:02 Headset", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECTED", "Device BB:BB:BB:BB:BB:02 Headset", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Headset");

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "disconnect BB:BB:BB:BB:BB:02", 5000));
  g_assert_cmpint (subscribe_count, ==, 2);
  emit_us = g_get_monotonic_time ();
  test_emit_connected ("BB:BB:BB:BB:BB:02", FALSE);
  g_assert_true (wait_label (qs->bt_status, "Disconnected from Headset.", FALSE,
                             5000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 500);
  g_assert_true (log_has (log, "disconnect BB:BB:BB:BB:BB:02"));
  g_assert_cmpuint (log_count (log, "info BB:BB:BB:BB:BB:02"), ==, 1);
  row = find_row (qs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Paired");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "On");

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* Regressão do bug reportado: o verbo falha mas o BlueZ conecta mesmo
 * assim. A falha do verbo não pode falhar de imediato: a assinatura fica
 * até o evento ou o fallback. */
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
  g_setenv ("QS_FAKE_BT_CONNECT_FAIL", "1", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "1500", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_STATE_EARLY", "1", TRUE);
  g_setenv ("QS_BT_SIGNAL_TIMEOUT_SECS", "1", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  run_loop_ms (300);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_false (log_has (log, "Failed"));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* O `connect` pode bloquear pelo timeout de página do BlueZ: o evento
 * fecha o link com o verbo ainda em execução, sem esperar o watchdog. */
static void
test_link_connect_slow_but_ok (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

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
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2500", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  emit_us = g_get_monotonic_time ();
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 500);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);

  test_subscribe_restore ();
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
  g_setenv ("QS_FAKE_BT_KEEP_STATE", "1", TRUE);
  g_setenv ("QS_BT_SIGNAL_TIMEOUT_SECS", "1", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  run_loop_ms (300);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  /* Sem sinal, o fallback com 3 leituras espaçadas acusa a falha
   * com detalhe: 1 info inicial do toggle + 3 do fallback. */
  g_assert_true (wait_label (qs->bt_status,
                             "Failed to connect Buds: Failed to connect: "
                             "org.bluez.Error.Failed", FALSE, 8000));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 4);
  assert_file_text (state, "no\n");
  run_loop_ms (2000);

  test_subscribe_restore ();
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
  gchar         *state;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();

  bt_cancel_all ();
  bt_cancel_all ();
  run_loop_ms (200);
  g_assert_cmpuint (log_count (log, "connect"), ==, 0);
  g_assert_cmpuint (log_count (log, "info"), ==, 0);

  free_fixture (qs, stack, log);

  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-cancel", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2500", TRUE);
  g_setenv ("QS_BT_SIGNAL_TIMEOUT_SECS", "1", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  bt_cancel_all ();
  run_loop_ms (300);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  run_loop_ms (3500);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  g_assert_cmpuint (log_count (log, "connect 11:22:33:44:55:66"), ==, 1);

  test_subscribe_restore ();
  g_free (state);
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
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "1200", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  gtk_container_remove (GTK_CONTAINER (qs->bt_list), row);
  g_assert_cmpuint (bt_row_count (qs), ==, 0);
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 1);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_sub)), ==, "Buds");

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_event_fallback_no_signal (void)
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
  state = g_build_filename (tmp_root, "bt-state-fallback", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_KEEP_STATE", "1", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "1500", TRUE);
  g_setenv ("QS_BT_SIGNAL_TIMEOUT_SECS", "1", TRUE);
  enable_connect_success ();
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  g_assert_cmpint (subscribe_count, ==, 2);
  run_loop_ms (500);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  g_assert_true (wait_label (qs->bt_status, "Failed to connect Buds:",
                             TRUE, 8000));
  /* 1 info inicial do toggle + 3 leituras do fallback espaçadas. */
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 4);
  assert_file_text (state, "no\n");
  run_loop_ms (1500);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 4);
  run_loop_ms (2000);

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_event_opposite_ignored (void)
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
  state = g_build_filename (tmp_root, "bt-state-opposite", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2000", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  test_emit_connected ("11:22:33:44:55:66", FALSE);
  run_loop_ms (400);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);
  test_emit_without_connected ("11:22:33:44:55:66");
  run_loop_ms (300);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==, 1);

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_event_latency (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-latency", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2000", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  emit_us = g_get_monotonic_time ();
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE, 5000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 500);
  g_message ("bt-event-latency-ms=%u", latency_ms);

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

static void
test_link_event_dead_no_touch (void)
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
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2000", TRUE);
  g_setenv ("QS_BT_SIGNAL_TIMEOUT_SECS", "2", TRUE);
  enable_connect_success ();
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Connecting…");
  qs->dead = TRUE;
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  run_loop_ms (800);
  g_assert_true (qs->dead);

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

typedef struct
{
  QuickSettings *qs;
  const gchar   *mac;
} RowWait;

static gboolean
row_connected_ready (gpointer data)
{
  RowWait *w = data;
  GList   *rows;
  GList   *l;
  gboolean hit = FALSE;

  rows = gtk_container_get_children (GTK_CONTAINER (w->qs->bt_list));
  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *m;

      if (!GTK_IS_LIST_BOX_ROW (l->data))
        continue;
      m = g_object_get_data (G_OBJECT (l->data), "qs-mac");
      if (g_strcmp0 (m, w->mac) == 0)
        {
          hit = GPOINTER_TO_INT (
            g_object_get_data (G_OBJECT (l->data), "qs-connected"));
          break;
        }
    }
  g_list_free (rows);
  return hit;
}

static gboolean
wait_row_connected (QuickSettings *qs,
                    const gchar   *mac,
                    guint          timeout_ms)
{
  RowWait w = { qs, mac };

  return wait_until (row_connected_ready, &w, timeout_ms);
}

/* RF-001: mudança externa via watch persistente da página repinta a
 * row sem Scan em até 2 s. Emite Paired+Connected sobre o watch da
 * página capturado via bt_signal_subscribe_fn; sem emissão a row
 * permanece inalterada (sinal de falha). */
static void
test_page_watch_external_update (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-pagewatch", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpint (subscribe_count, ==, 1);
  g_assert_nonnull (captured_page_watch);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_false (row_connected (row));
  g_assert_cmpstr (row_state (row), !=, "Connected");

  emit_us = g_get_monotonic_time ();
  test_emit_page_properties ("11:22:33:44:55:66", TRUE, TRUE);
  g_assert_true (wait_row_connected (qs, "11:22:33:44:55:66", 2000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 2000);
  g_assert_cmpuint (latency_ms, <, 500);
  g_message ("bt-event-latency-ms=%u", latency_ms);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  g_assert_true (row_connected (row));
  g_assert_cmpstr (row_state (row), ==, "Connected");

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* RF-004: clique engolido por busy mostra feedback em até 200 ms
 * sem spawn extra. O segundo clique sobre o mesmo MAC em curso
 * escreve "Ocupado, aguarde…" e não dispara novo connect/info. */
static void
test_toggle_busy_feedback (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  guint          connects_before;
  guint          infos_before;
  gint64         click_us;
  gint64         feedback_us;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-busy", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  enable_connect_success ();
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "2000", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 5000));
  connects_before = log_count (log, "connect 11:22:33:44:55:66");
  infos_before = log_count (log, "info 11:22:33:44:55:66");
  click_us = g_get_monotonic_time ();
  g_signal_emit_by_name (qs->bt_list, "row-selected", row);
  g_assert_true (wait_label (qs->bt_status, "Ocupado, aguarde…", FALSE,
                             1000));
  feedback_us = g_get_monotonic_time ();
  g_assert_cmpuint ((guint) ((feedback_us - click_us) / 1000), <, 1000);
  g_assert_cmpuint (log_count (log, "connect 11:22:33:44:55:66"), ==,
                    connects_before);
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), ==,
                    infos_before);

  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE,
                             5000));

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* RF-003: rótulos por verbo. O fluxo pair+trust+connect exibe um
 * rótulo de verbo (Pairing…/Trusting…/Connecting…) e nunca volta
 * para Loading… no meio do link. */
static void
test_link_verb_labels (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  const gchar   *label;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-verblabel", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "no", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "no", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECT_DELAY_MS", "1500", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  g_signal_emit_by_name (qs->bt_list, "row-activated", row);
  g_assert_true (wait_log_struct (log, "connect 11:22:33:44:55:66", 8000));
  label = gtk_label_get_text (GTK_LABEL (qs->bt_status));
  g_assert_true (g_strcmp0 (label, "Pairing…") == 0
                 || g_strcmp0 (label, "Trusting…") == 0
                 || g_strcmp0 (label, "Connecting…") == 0);
  g_assert_true (log_has (log, "pair 11:22:33:44:55:66"));
  g_assert_true (log_has (log, "trust 11:22:33:44:55:66"));
  test_emit_connected ("11:22:33:44:55:66", TRUE);
  g_assert_true (wait_label (qs->bt_status, "Connected to Buds.", FALSE,
                             5000));

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* RF-006: trust standalone por row executa o verbo e, após reload
 * via `info`, a row exibe trusted com `info MAC` confirmando
 * Trusted yes persistido. */
static void
test_trust_row_success (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  gchar         *state;
  gint64         emit_us;
  gint64         done_us;
  guint          latency_ms;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  state = g_build_filename (tmp_root, "bt-state-trust", NULL);
  create_state_file (state, "no");
  g_setenv ("QS_FAKE_BT_STATE", state, TRUE);
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpint (subscribe_count, ==, 1);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  emit_us = g_get_monotonic_time ();
  bt_trust_row (qs, GTK_LIST_BOX_ROW (row));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Trusting…");
  g_assert_true (wait_label (qs->bt_status, "Trusted Buds.", FALSE, 5000));
  done_us = g_get_monotonic_time ();
  latency_ms = (guint) ((done_us - emit_us) / 1000);
  g_assert_cmpuint (latency_ms, <, 5000);
  g_message ("bt-event-latency-ms=%u", latency_ms);
  g_assert_true (log_has (log, "trust 11:22:33:44:55:66"));
  g_assert_cmpuint (log_count (log, "info 11:22:33:44:55:66"), >=, 1);
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  test_subscribe_restore ();
  g_free (state);
  free_fixture (qs, stack, log);
}

/* RF-005: Agent1 restrito ao alvo. O segundo device path é
 * rejeitado: qs_bt_agent_path_matches devolve FALSE para o
 * estranho e TRUE para o alvo (mesmo com outro adaptador). */
static void
test_agent_path_negative (void)
{
  gchar *target;

  target = qs_bt_agent_device_path_for_mac ("11:22:33:44:55:66");
  g_assert_nonnull (target);
  g_assert_true (qs_bt_agent_path_matches (
    target, "/org/bluez/hci0/dev_11_22_33_44_55_66"));
  g_assert_true (qs_bt_agent_path_matches (
    target, "/org/bluez/hci1/dev_11_22_33_44_55_66"));
  g_assert_false (qs_bt_agent_path_matches (
    target, "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_01"));
  g_assert_false (qs_bt_agent_path_matches (target, NULL));
  g_assert_false (qs_bt_agent_path_matches (NULL, target));
  g_assert_false (qs_bt_agent_path_matches ("", ""));
  g_assert_false (qs_bt_agent_is_target (NULL,
    "/org/bluez/hci0/dev_11_22_33_44_55_66"));
  g_assert_null (qs_bt_agent_device_path_for_mac (NULL));
  g_assert_null (qs_bt_agent_device_path_for_mac ("curto"));
  g_assert_null (qs_bt_agent_get_target (NULL));
  g_assert_null (qs_bt_agent_ask_pin (NULL, NULL));
  g_free (target);
}

static gboolean
fake_rename_dbus_ok (const gchar *mac,
                     const gchar *alias,
                     GError     **error)
{
  (void) mac;
  (void) alias;
  (void) error;
  return TRUE;
}

/* RF-007: rename via set-alias repinta a row com o novo nome; com
 * falha do primário, o fallback D-Bus nomeado cobre e ainda
 * conclui como Renamed. */
static void
test_rename_apply_success (void)
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *row;
  gchar         *log;
  const gchar   *name;

  if (skip_no_display ())
    return;
  reset_fake_env ();
  set_path_prefixed ();
  log = new_log ();
  qs = qs_new ();
  stack = make_fixture (qs, &stack);
  bt_free_caches ();
  g_setenv ("QS_FAKE_BT_DEVICES", "Device 11:22:33:44:55:66 Buds", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_PAIRED", "yes", TRUE);
  g_setenv ("QS_FAKE_BT_INFO_TRUSTED", "yes", TRUE);
  test_subscribe_install ();

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);

  bt_rename_apply (qs, "11:22:33:44:55:66", "FoneNovo");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->bt_status)), ==,
                   "Renaming…");
  g_assert_true (wait_label (qs->bt_status, "Renamed to FoneNovo.", FALSE,
                             5000));
  g_assert_true (log_has (log, "set-alias 11:22:33:44:55:66"));
  row = find_row (qs, "11:22:33:44:55:66");
  g_assert_nonnull (row);
  name = g_object_get_data (G_OBJECT (row), "qs-name");
  g_assert_cmpstr (name, ==, "FoneNovo");

  test_subscribe_restore ();
  free_fixture (qs, stack, log);
}

static void
test_rename_fallback_dbus (void)
{
  BtRenameStatus st;
  GError        *error = NULL;

  reset_fake_env ();
  set_path_prefixed ();
  g_setenv ("QS_FAKE_BT_ALIAS_FAIL", "1", TRUE);
  bt_rename_set_dbus_fn (fake_rename_dbus_ok);
  st = bt_rename_device ("11:22:33:44:55:66", "FoneFallback");
  g_assert_cmpint (st, ==, BT_RENAME_OK);
  bt_rename_set_dbus_fn (NULL);
  g_assert_false (bt_rename_via_dbus (NULL, "x", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (bt_rename_via_dbus ("11:22:33:44:55:66", NULL, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_unsetenv ("QS_FAKE_BT_ALIAS_FAIL");
  test_subscribe_restore ();
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
            "Device AA:BB:CC:DD:EE:13 \n"
            "Device AA:BB:CC:DD:EE:14 aa:bb:cc:dd:ee:14\n"
            "Device AA:BB:CC:DD:EE:15 Unknown\n"
            "Device AA:BB:CC:DD:EE:16 (unknown)\n"
            "Device AA:BB:CC:DD:EE:17    \n"
            "Device AA:BB:CC:DD:EE:18\r\n"
            "Device 11:22:33:44:55:66 Buds",
            TRUE);
  /* Rota bt_apply_filtered_devices: furos nos listados filtrados também
   * precisam continuar ocultos. */
  g_setenv ("QS_FAKE_BT_PAIRED",
            "Device AA:BB:CC:DD:EE:21 Unknown\n"
            "Device AA:BB:CC:DD:EE:22 aa:bb:cc:dd:ee:22",
            TRUE);
  g_setenv ("QS_FAKE_BT_TRUSTED",
            "Device AA:BB:CC:DD:EE:23 \n"
            "Device AA:BB:CC:DD:EE:24 (UNKNOWN)",
            TRUE);
  g_setenv ("QS_FAKE_BT_CONNECTED",
            "Device AA:BB:CC:DD:EE:25    \n"
            "Device AA:BB:CC:DD:EE:26\r",
            TRUE);

  bt_show_page (qs);
  g_assert_true (wait_load_settled (qs, 5000));
  g_assert_cmpuint (bt_row_count (qs), ==, 1);
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:11"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:12"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:13"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:14"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:15"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:16"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:17"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:18"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:21"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:22"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:23"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:24"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:25"));
  g_assert_null (find_row (qs, "AA:BB:CC:DD:EE:26"));
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
  g_test_add_func ("/bt-flow/link_event_fallback",
                   test_link_event_fallback_no_signal);
  g_test_add_func ("/bt-flow/link_event_opposite",
                   test_link_event_opposite_ignored);
  g_test_add_func ("/bt-flow/link_event_latency", test_link_event_latency);
  g_test_add_func ("/bt-flow/link_event_dead", test_link_event_dead_no_touch);
  g_test_add_func ("/bt-flow/page_watch_external", test_page_watch_external_update);
  g_test_add_func ("/bt-flow/toggle_busy", test_toggle_busy_feedback);
  g_test_add_func ("/bt-flow/verb_labels", test_link_verb_labels);
  g_test_add_func ("/bt-flow/trust_row", test_trust_row_success);
  g_test_add_func ("/bt-flow/agent_negative", test_agent_path_negative);
  g_test_add_func ("/bt-flow/rename_apply", test_rename_apply_success);
  g_test_add_func ("/bt-flow/rename_fallback", test_rename_fallback_dbus);
  g_test_add_func ("/bt-flow/hide_nameless", test_hide_nameless_devices);
  g_test_add_func ("/bt-flow/teardown", test_teardown_idempotent);

  return g_test_run ();
}
