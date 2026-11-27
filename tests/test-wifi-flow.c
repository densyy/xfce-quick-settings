/* test-wifi-flow.c — fluxos assíncronos de Wi-Fi (qs-wifi.c).
 *
 * Usa somente tests/fakebin/nmcli via PATH e XDG temporário; nenhum comando
 * real de rede é executado. Cobre load (cache/scan/erro/dead), lista diff,
 * conexão (perfil salvo, senha, falha, dead), cache de perfis e página.
 * Precisa de display para os widgets; pula sem display.
 */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-net.h"
#include "qs-cache.h"
#include "qs-ui.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
static gchar   *old_path = NULL;
static gchar   *empty_dir = NULL;
static gchar   *log_path = NULL;
static gint     dialog_response = GTK_RESPONSE_NONE;
static const gchar *dialog_password = NULL;
static gint     dialog_tries = 0;

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

static gboolean
dialog_probe (gpointer data)
{
  GList *tops = gtk_window_list_toplevels ();
  GList *l;
  (void) data;

  for (l = tops; l != NULL; l = l->next)
    {
      if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (GTK_WIDGET (l->data)))
        {
          GtkWidget *dlg = GTK_WIDGET (l->data);

          if (dialog_password != NULL)
            {
              GList *kids = gtk_container_get_children (GTK_CONTAINER (
                               gtk_dialog_get_content_area (GTK_DIALOG (dlg))));
              GList *k;

              for (k = kids; k != NULL; k = k->next)
                {
                  if (GTK_IS_ENTRY (k->data))
                    gtk_entry_set_text (GTK_ENTRY (k->data), dialog_password);
                }
              g_list_free (kids);
            }
          gtk_dialog_response (GTK_DIALOG (dlg), dialog_response);
          g_list_free (tops);
          return G_SOURCE_REMOVE;
        }
    }
  g_list_free (tops);
  dialog_tries++;
  return dialog_tries < 60 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
run_loop_ms (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

typedef gboolean (*WaitFunc) (gpointer data);

static gboolean
wait_until (WaitFunc fn,
            gpointer data,
            guint    timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (fn (data))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
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
use_fake_path (void)
{
  gchar *path = g_strconcat (fakebin, ":", old_path, NULL);

  g_setenv ("PATH", path, TRUE);
  g_free (path);
}

static void
use_empty_path (void)
{
  g_setenv ("PATH", empty_dir, TRUE);
}

static void
reset_env (void)
{
  use_fake_path ();
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_setenv ("QS_FAKE_NMCLI_LIST", "", TRUE);
  g_setenv ("QS_FAKE_NMCLI_PROFILES", "", TRUE);
  g_unsetenv ("QS_FAKE_NMCLI_SCAN_FAIL");
  g_unsetenv ("QS_FAKE_NMCLI_CONNECT_FAIL");
  g_unsetenv ("QS_FAKE_NMCLI_CONNECT_SLEEP");
  g_remove (log_path);
}

static gchar *
read_log (void)
{
  gchar *text = NULL;

  if (!g_file_get_contents (log_path, &text, NULL, NULL))
    return g_strdup ("");
  return text;
}

static gboolean
log_contains (const gchar *needle)
{
  gchar   *text = read_log ();
  gboolean hit = strstr (text, needle) != NULL;

  g_free (text);
  return hit;
}

static guint
log_count (const gchar *needle)
{
  gchar *text = read_log ();
  gchar *pos = text;
  guint  count = 0;
  gsize  len = strlen (needle);

  while ((pos = strstr (pos, needle)) != NULL)
    {
      count++;
      pos += len;
    }
  g_free (text);
  return count;
}

static gboolean
wait_log (const gchar *needle,
          guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_contains (needle))
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return log_contains (needle);
}

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *stack;
  GtkWidget     *back;
} Fixture;

static void
fixture_init (Fixture *f)
{
  f->qs = qs_new ();
  f->stack = gtk_stack_new ();
  wifi_build_page (f->stack, f->qs, &f->back);
  gtk_widget_show_all (f->stack);
  f->qs->stack = f->stack;
  f->qs->wifi_tile = gtk_button_new ();
  f->qs->wifi_sub = gtk_label_new ("");
}

static void
fixture_clear (Fixture *f)
{
  qs_cache_stop ();
  gtk_widget_destroy (f->stack);
  gtk_widget_destroy (f->qs->wifi_tile);
  gtk_widget_destroy (f->qs->wifi_sub);
  qs_unref (f->qs);
}

static GtkWidget *
find_row (QuickSettings *qs,
          const gchar   *ssid)
{
  GList     *kids = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
  GList     *l;
  GtkWidget *row = NULL;

  for (l = kids; l != NULL; l = l->next)
    {
      if (GTK_IS_LIST_BOX_ROW (l->data)
          && g_strcmp0 (g_object_get_data (G_OBJECT (l->data), "qs-ssid"),
                        ssid) == 0)
        {
          row = GTK_WIDGET (l->data);
          break;
        }
    }
  g_list_free (kids);
  return row;
}

static GtkWidget *
find_row_for_ssid (QuickSettings *qs,
                   const gchar   *ssid)
{
  return find_row (qs, ssid);
}

typedef struct
{
  QuickSettings *qs;
  const gchar   *ssid;
} RowWait;

static gboolean
row_exists (gpointer data)
{
  RowWait *w = data;

  return find_row (w->qs, w->ssid) != NULL;
}

static GtkWidget *
wait_row (QuickSettings *qs,
          const gchar   *ssid)
{
  RowWait w = { qs, ssid };

  wait_until (row_exists, &w, 10000);
  return find_row_for_ssid (qs, ssid);
}

typedef struct
{
  GtkWidget   *label;
  const gchar *text;
} LabelWait;

static gboolean
label_matches (gpointer data)
{
  LabelWait *w = data;

  return g_strcmp0 (gtk_label_get_text (GTK_LABEL (w->label)), w->text) == 0;
}

static gboolean
wait_label (GtkWidget   *label,
            const gchar *text)
{
  LabelWait w = { label, text };

  return wait_until (label_matches, &w, 10000);
}

static gboolean
wait_button_label (GtkWidget   *button,
                   const gchar *text)
{
  guint waited = 0;

  while (waited < 10000)
    {
      if (g_strcmp0 (gtk_button_get_label (GTK_BUTTON (button)), text) == 0)
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return g_strcmp0 (gtk_button_get_label (GTK_BUTTON (button)), text) == 0;
}

static guint
list_children (QuickSettings *qs)
{
  GList *kids = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
  guint  n = g_list_length (kids);

  g_list_free (kids);
  return n;
}

static const gchar *
first_row_label (QuickSettings *qs)
{
  GList     *kids = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
  GList     *l;
  const gchar *text = NULL;

  for (l = kids; l != NULL && text == NULL; l = l->next)
    {
      if (GTK_IS_LIST_BOX_ROW (l->data))
        {
          GtkWidget *child = gtk_bin_get_child (GTK_BIN (l->data));

          if (child != NULL && GTK_IS_LABEL (child))
            text = gtk_label_get_text (GTK_LABEL (child));
          else if (child != NULL && GTK_IS_CONTAINER (child))
            {
              GList *inner = gtk_container_get_children (GTK_CONTAINER (child));
              GList *i;

              for (i = inner; i != NULL; i = i->next)
                {
                  if (GTK_IS_LABEL (i->data))
                    {
                      text = gtk_label_get_text (GTK_LABEL (i->data));
                      break;
                    }
                }
              g_list_free (inner);
            }
        }
    }
  g_list_free (kids);
  return text;
}

typedef struct
{
  QuickSettings *qs;
  const gchar   *text;
} PlaceholderWait;

static gboolean
placeholder_is (gpointer data)
{
  PlaceholderWait *w = data;

  return g_strcmp0 (first_row_label (w->qs), w->text) == 0;
}

static gboolean
wait_placeholder (QuickSettings *qs,
                  const gchar   *text)
{
  PlaceholderWait w = { qs, text };

  return wait_until (placeholder_is, &w, 10000);
}

static guint
row_images (GtkWidget *row)
{
  GtkWidget *hbox = gtk_bin_get_child (GTK_BIN (row));
  GList     *kids;
  GList     *l;
  guint      n = 0;

  if (hbox == NULL || !GTK_IS_CONTAINER (hbox))
    return 0;
  kids = gtk_container_get_children (GTK_CONTAINER (hbox));
  for (l = kids; l != NULL; l = l->next)
    {
      if (GTK_IS_IMAGE (l->data))
        n++;
    }
  g_list_free (kids);
  return n;
}

static const gchar *
row_meta (GtkWidget *row)
{
  GtkWidget *hbox = gtk_bin_get_child (GTK_BIN (row));
  GList     *kids;
  GList     *l;
  const gchar *text = NULL;
  gint       labels = 0;

  if (hbox == NULL || !GTK_IS_CONTAINER (hbox))
    return NULL;
  kids = gtk_container_get_children (GTK_CONTAINER (hbox));
  for (l = kids; l != NULL; l = l->next)
    {
      if (GTK_IS_LABEL (l->data))
        {
          labels++;
          if (labels == 2)
            text = gtk_label_get_text (GTK_LABEL (l->data));
        }
    }
  g_list_free (kids);
  return text;
}

static void
activate_row (QuickSettings *qs,
              GtkWidget     *row)
{
  g_signal_emit_by_name (qs->wifi_list, "row-activated", row);
}

static void
activate_with_dialog (QuickSettings *qs,
                      GtkWidget     *row,
                      gint           response,
                      const gchar   *password)
{
  dialog_response = response;
  dialog_password = password;
  dialog_tries = 0;
  g_timeout_add (100, dialog_probe, NULL);
  activate_row (qs, row);
  dialog_response = GTK_RESPONSE_NONE;
  dialog_password = NULL;
}

/* --- testes --- */

static void
test_free_cache_before_init (void)
{
  wifi_free_cache ();
  wifi_free_cache ();
}

static void
test_show_page_available (void)
{
  Fixture *f;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST",
            "*:Home:80:WPA2\n:Cafe:60:WPA2\n:Open:40:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f->stack)),
                   ==, "wifi");
  g_assert_nonnull (wait_row (f->qs, "Home"));
  g_assert_nonnull (find_row (f->qs, "Cafe"));
  g_assert_nonnull (find_row (f->qs, "Open"));
  g_assert_cmpuint (list_children (f->qs), ==, 3);
  g_assert_true (f->qs->wifi_on);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f->qs->wifi_switch)));
  g_assert_cmpstr (f->qs->wifi_ssid, ==, "Home");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->wifi_sub)), ==, "Home");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->wifi_status)), ==, "");
  g_assert_true (gtk_widget_get_sensitive (f->qs->wifi_scan));
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (f->qs->wifi_scan)),
                   ==, "Scan for networks");
  g_assert_true (log_contains ("device wifi list --rescan no"));
  g_assert_true (log_contains ("radio wifi"));
  fixture_clear (f);
  g_free (f);
}

static void
test_show_page_unavailable (void)
{
  Fixture *f;

  if (skip_no_display ())
    return;
  reset_env ();
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  use_empty_path ();
  g_remove (log_path);
  wifi_show_page (f->qs);
  g_assert_false (log_contains ("device wifi list"));
  use_fake_path ();
  fixture_clear (f);
  g_free (f);
}

static void
test_load_empty_placeholder (void)
{
  Fixture *f;

  if (skip_no_display ())
    return;
  reset_env ();
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  g_assert_true (wait_placeholder (f->qs, "No networks found."));
  g_assert_cmpuint (list_children (f->qs), ==, 1);
  g_assert_true (f->qs->wifi_on);
  g_assert_true (gtk_widget_get_sensitive (f->qs->wifi_scan));

  g_setenv ("QS_FAKE_WIFI_STATE", "disabled", TRUE);
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_assert_false (gtk_widget_get_sensitive (f->qs->wifi_scan));
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (f->qs->wifi_scan)),
                   ==, "Scanning…");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->wifi_status)),
                   ==, "Scanning for networks…");
  g_assert_true (wait_placeholder (f->qs, "Wi-Fi is off."));
  g_assert_false (f->qs->wifi_on);
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f->qs->wifi_switch)));
  g_assert_true (gtk_widget_get_sensitive (f->qs->wifi_scan));
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (f->qs->wifi_scan)),
                   ==, "Scan for networks");
  g_assert_true (log_contains ("device wifi list --rescan yes"));
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_setenv ("QS_FAKE_NMCLI_SCAN_FAIL", "scan failed", TRUE);
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_assert_true (wait_placeholder (f->qs, "No networks found."));
  g_assert_true (f->qs->wifi_on);
  g_assert_true (gtk_widget_get_sensitive (f->qs->wifi_scan));
  g_unsetenv ("QS_FAKE_NMCLI_SCAN_FAIL");
  fixture_clear (f);
  g_free (f);
}

static void
test_load_patch_rows (void)
{
  Fixture *f;
  GtkWidget *home;
  GtkWidget *ghost;
  guint waited;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST",
            "*:Home:80:WPA2\n:Cafe:70:WPA2\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  home = wait_row (f->qs, "Home");
  g_assert_nonnull (home);
  g_assert_cmpuint (row_images (home), ==, 2);

  ghost = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (ghost), "qs-ssid",
                          g_strdup ("Ghost"), g_free);
  gtk_container_add (GTK_CONTAINER (f->qs->wifi_list), ghost);

  g_setenv ("QS_FAKE_NMCLI_LIST",
            ":Home:20:--\n:Cafe:70:WPA2\n:Beta:50:WPA2\n:Ghost:10:--\n",
            TRUE);
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_assert_nonnull (wait_row (f->qs, "Beta"));
  home = find_row (f->qs, "Home");
  g_assert_nonnull (home);
  g_assert_cmpuint (row_images (home), ==, 1);
  g_assert_cmpstr (row_meta (home), ==, "20%");

  g_setenv ("QS_FAKE_NMCLI_LIST", "*:Home:90:WPA2\n", TRUE);
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  waited = 0;
  for (;;)
    {
      GtkWidget *h = find_row (f->qs, "Home");

      if (h != NULL && row_images (h) == 2
          && g_strcmp0 (row_meta (h), "90%") == 0)
        break;
      if (waited >= 10000)
        break;
      run_loop_ms (50);
      waited += 50;
    }
  home = find_row (f->qs, "Home");
  g_assert_nonnull (home);
  g_assert_cmpuint (row_images (home), ==, 2);
  g_assert_cmpstr (row_meta (home), ==, "90%");
  g_assert_null (find_row (f->qs, "Cafe"));
  g_assert_null (find_row (f->qs, "Beta"));
  g_assert_null (find_row (f->qs, "Ghost"));
  g_assert_cmpuint (list_children (f->qs), ==, 1);
  fixture_clear (f);
  g_free (f);
}

static void
test_load_scan_guard (void)
{
  Fixture *f;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":One:50:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  g_assert_nonnull (wait_row (f->qs, "One"));
  g_remove (log_path);
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_assert_true (wait_button_label (f->qs->wifi_scan, "Scan for networks"));
  run_loop_ms (100);
  g_assert_cmpuint (log_count ("device wifi list"), ==, 1);
  fixture_clear (f);
  g_free (f);
}

static void
test_load_error_and_dead (void)
{
  Fixture *f;

  if (skip_no_display ())
    return;
  reset_env ();
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  use_empty_path ();
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  g_assert_true (wait_label (f->qs->wifi_status, "Could not list networks."));
  g_assert_true (gtk_widget_get_sensitive (f->qs->wifi_scan));
  g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (f->qs->wifi_scan)),
                   ==, "Scan for networks");
  use_fake_path ();
  fixture_clear (f);
  g_free (f);

  reset_env ();
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  f->qs->dead = TRUE;
  g_signal_emit_by_name (f->qs->wifi_scan, "clicked");
  run_loop_ms (500);
  fixture_clear (f);
  g_free (f);
}

static void
test_row_activated_defensive (void)
{
  Fixture *f;
  GtkWidget *row;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Open:70:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  g_assert_nonnull (wait_row (f->qs, "Open"));

  g_remove (log_path);
  row = make_placeholder_row ("placeholder");
  gtk_container_add (GTK_CONTAINER (f->qs->wifi_list), row);
  gtk_widget_show_all (f->qs->wifi_list);
  activate_row (f->qs, row);

  row = gtk_list_box_row_new ();
  gtk_container_add (GTK_CONTAINER (f->qs->wifi_list), row);
  gtk_widget_show_all (f->qs->wifi_list);
  activate_row (f->qs, row);
  run_loop_ms (200);
  g_assert_false (log_contains ("device wifi connect"));
  g_assert_false (log_contains ("connection show"));
  fixture_clear (f);
  g_free (f);
}

static void
test_switch_reload (void)
{
  Fixture *f;
  guint waited;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_WIFI_STATE", "disabled", TRUE);
  g_setenv ("QS_FAKE_NMCLI_LIST", ":One:50:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  g_assert_nonnull (wait_row (f->qs, "One"));
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f->qs->wifi_switch)));

  g_remove (log_path);
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  gtk_switch_set_active (GTK_SWITCH (f->qs->wifi_switch), TRUE);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->wifi_status)),
                   ==, "Turning Wi-Fi on…");
  g_assert_true (wait_log ("radio wifi on", 10000));
  waited = 0;
  while (waited < 3000 && log_count ("device wifi list") < 1)
    {
      run_loop_ms (100);
      waited += 100;
    }
  g_assert_cmpuint (log_count ("device wifi list"), >=, 1);
  run_loop_ms (200);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f->qs->wifi_switch)));
  g_assert_true (f->qs->wifi_on);

  g_setenv ("QS_FAKE_WIFI_STATE", "disabled", TRUE);
  gtk_switch_set_active (GTK_SWITCH (f->qs->wifi_switch), FALSE);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->wifi_status)),
                   ==, "Turning Wi-Fi off…");
  g_assert_true (wait_log ("radio wifi off", 10000));
  waited = 0;
  while (waited < 3000 && log_count ("device wifi list") < 2)
    {
      run_loop_ms (100);
      waited += 100;
    }
  g_assert_cmpuint (log_count ("device wifi list"), >=, 2);
  run_loop_ms (300);
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f->qs->wifi_switch)));
  g_assert_false (f->qs->wifi_on);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_open_commands (void)
{
  Fixture *f;
  GtkWidget *open;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Open:70:--\n:Home:80:WPA2\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  open = wait_row (f->qs, "Open");
  g_assert_nonnull (open);

  activate_row (f->qs, open);
  g_assert_true (wait_log ("device wifi connect Open", 10000));
  run_loop_ms (300);
  /* Load pre-warm + reload pós-connect: 2 queries de perfis. Antes do
   * pre-warm no load worker (D-003) este total era 1. */
  g_assert_cmpuint (log_count ("connection show"), ==, 2);
  g_assert_false (f->qs->scanning_wifi);

  g_setenv ("QS_FAKE_NMCLI_PROFILES", "Open:802-11-wireless\n", TRUE);
  /* Perfil criado fora da janela do reload pós-connect: o snapshot
   * pre-warmed ainda não o contém (TTL 60s), então invalida para o worker
   * reconsultar, como faria após expirar. */
  wifi_free_cache ();
  open = find_row (f->qs, "Open");
  g_assert_nonnull (open);
  activate_row (f->qs, open);
  g_assert_true (wait_log ("connection up Open", 10000));
  run_loop_ms (300);
  /* #3 worker re-query após invalidação + #4 reload pós-connect. */
  g_assert_cmpuint (log_count ("connection show"), ==, 4);
  g_assert_false (f->qs->scanning_wifi);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_saved_cache_hit (void)
{
  Fixture *f;
  GtkWidget *open;
  GtkWidget *home;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Open:70:--\n:Home:80:WPA2\n", TRUE);
  g_setenv ("QS_FAKE_NMCLI_PROFILES", "Home:802-11-wireless\n", TRUE);
  g_setenv ("QS_FAKE_NMCLI_CONNECT_SLEEP", "2", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  open = wait_row (f->qs, "Open");
  home = wait_row (f->qs, "Home");
  g_assert_nonnull (open);
  g_assert_nonnull (home);

  activate_row (f->qs, open);
  g_assert_true (wait_log ("device wifi connect Open", 5000));
  activate_row (f->qs, home);
  g_assert_true (wait_log ("connection up Home", 5000));
  g_assert_false (log_contains ("Wi-Fi password"));
  run_loop_ms (2500);
  g_assert_false (f->qs->scanning_wifi);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_saved_first_click_no_dialog (void)
{
  Fixture *f;
  GtkWidget *casa;

  if (skip_no_display ())
    return;
  reset_env ();
  /* Cold start: sem o pre-warm do load worker o cache segue inválido e o
   * primeiro clique numa rede salva abriria o dialog de senha (D-003). */
  wifi_free_cache ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Casa:70:WPA2\n", TRUE);
  g_setenv ("QS_FAKE_NMCLI_PROFILES", "Casa:802-11-wireless\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  casa = wait_row (f->qs, "Casa");
  g_assert_nonnull (casa);
  /* O load pre-warmou o saved-set off-UI antes de pintar a lista. */
  g_assert_true (wait_log ("connection show", 10000));
  g_remove (log_path);
  /* Clique sem senha: rede salva não pode abrir dialog. O probe CANCEL só
   * existe para não travar se o dialog abrir (e aí nenhum up acontece). */
  activate_with_dialog (f->qs, casa, GTK_RESPONSE_CANCEL, NULL);
  g_assert_true (wait_log ("connection up Casa", 10000));
  g_assert_false (log_contains ("device wifi connect Casa"));
  run_loop_ms (300);
  g_assert_false (f->qs->scanning_wifi);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_unknown_prompts_password (void)
{
  Fixture *f;
  GtkWidget *nova;

  if (skip_no_display ())
    return;
  reset_env ();
  wifi_free_cache ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Nova:60:WPA2\n", TRUE);
  /* Sem QS_FAKE_NMCLI_PROFILES: SSID desconhecido mesmo após o pre-warm. */
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  nova = wait_row (f->qs, "Nova");
  g_assert_nonnull (nova);
  g_assert_true (wait_log ("connection show", 10000));
  g_remove (log_path);
  activate_with_dialog (f->qs, nova, GTK_RESPONSE_ACCEPT, "s3cret");
  g_assert_true (wait_log ("device wifi connect Nova password s3cret",
                           10000));
  g_assert_false (log_contains ("connection up Nova"));
  run_loop_ms (300);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_password_saved (void)
{
  Fixture *f;
  GtkWidget *home;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Home:80:WPA2\n", TRUE);
  g_setenv ("QS_FAKE_NMCLI_PROFILES", "Home:802-11-wireless\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  home = wait_row (f->qs, "Home");
  g_assert_nonnull (home);
  wifi_free_cache ();
  activate_with_dialog (f->qs, home, GTK_RESPONSE_ACCEPT, "s3cret");
  g_assert_true (wait_log ("connection up Home", 10000));
  run_loop_ms (300);
  g_assert_false (log_contains ("password s3cret"));
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_password_new (void)
{
  Fixture *f;
  GtkWidget *cafe;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Cafe:60:WPA2\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  cafe = wait_row (f->qs, "Cafe");
  g_assert_nonnull (cafe);
  wifi_free_cache ();
  activate_with_dialog (f->qs, cafe, GTK_RESPONSE_ACCEPT, "s3cret");
  g_assert_true (wait_log ("device wifi connect Cafe password s3cret", 10000));
  run_loop_ms (300);
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_password_cancel (void)
{
  Fixture *f;
  GtkWidget *cafe;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Cafe:60:WPA2\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  cafe = wait_row (f->qs, "Cafe");
  g_assert_nonnull (cafe);
  wifi_free_cache ();
  g_remove (log_path);
  activate_with_dialog (f->qs, cafe, GTK_RESPONSE_CANCEL, "s3cret");
  run_loop_ms (300);
  g_assert_false (log_contains ("device wifi connect"));
  g_assert_false (log_contains ("connection up"));
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_spawn_failure (void)
{
  Fixture *f;
  GtkWidget *open;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Open:70:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  open = wait_row (f->qs, "Open");
  g_assert_nonnull (open);
  use_empty_path ();
  activate_row (f->qs, open);
  g_assert_true (wait_label (f->qs->wifi_status, "Could not list networks."));
  g_assert_false (f->qs->scanning_wifi);
  use_fake_path ();
  fixture_clear (f);
  g_free (f);
}

static void
test_connect_dead (void)
{
  Fixture *f;
  GtkWidget *open;

  if (skip_no_display ())
    return;
  reset_env ();
  g_setenv ("QS_FAKE_NMCLI_LIST", ":Open:70:--\n", TRUE);
  f = g_new0 (Fixture, 1);
  fixture_init (f);
  wifi_show_page (f->qs);
  open = wait_row (f->qs, "Open");
  g_assert_nonnull (open);
  g_remove (log_path);
  activate_row (f->qs, open);
  f->qs->dead = TRUE;
  run_loop_ms (500);
  g_assert_true (log_contains ("device wifi connect Open"));
  fixture_clear (f);
  g_free (f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_log_set_always_fatal (G_LOG_FATAL_MASK);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-wifi-flow-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  fakebin = g_build_filename (g_get_current_dir (), "tests", "fakebin", NULL);
  empty_dir = g_build_filename (tmp_root, "empty", NULL);
  g_assert_cmpint (g_mkdir_with_parents (empty_dir, 0700), ==, 0);
  old_path = g_strdup (g_getenv ("PATH"));
  if (old_path == NULL)
    old_path = g_strdup ("/usr/bin:/bin");
  log_path = g_build_filename (tmp_root, "nmcli.log", NULL);
  g_setenv ("QS_FAKE_LOG", log_path, TRUE);

  g_test_add_func ("/wifi/flow/free_cache_before_init",
                   test_free_cache_before_init);
  g_test_add_func ("/wifi/flow/show_page_available",
                   test_show_page_available);
  g_test_add_func ("/wifi/flow/show_page_unavailable",
                   test_show_page_unavailable);
  g_test_add_func ("/wifi/flow/load_empty_placeholder",
                   test_load_empty_placeholder);
  g_test_add_func ("/wifi/flow/load_patch_rows", test_load_patch_rows);
  g_test_add_func ("/wifi/flow/load_scan_guard", test_load_scan_guard);
  g_test_add_func ("/wifi/flow/load_error_and_dead", test_load_error_and_dead);
  g_test_add_func ("/wifi/flow/row_activated_defensive",
                   test_row_activated_defensive);
  g_test_add_func ("/wifi/flow/switch_reload", test_switch_reload);
  g_test_add_func ("/wifi/flow/connect_open_commands",
                   test_connect_open_commands);
  g_test_add_func ("/wifi/flow/connect_saved_cache_hit",
                   test_connect_saved_cache_hit);
  g_test_add_func ("/wifi/flow/connect_saved_first_click_no_dialog",
                   test_connect_saved_first_click_no_dialog);
  g_test_add_func ("/wifi/flow/connect_unknown_prompts_password",
                   test_connect_unknown_prompts_password);
  g_test_add_func ("/wifi/flow/connect_password_saved",
                   test_connect_password_saved);
  g_test_add_func ("/wifi/flow/connect_password_new",
                   test_connect_password_new);
  g_test_add_func ("/wifi/flow/connect_password_cancel",
                   test_connect_password_cancel);
  g_test_add_func ("/wifi/flow/connect_spawn_failure",
                   test_connect_spawn_failure);
  g_test_add_func ("/wifi/flow/connect_dead", test_connect_dead);
  return g_test_run ();
}
