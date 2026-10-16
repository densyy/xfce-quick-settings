/* Testes de segurança do path de conexão Wi-Fi (regressão contra injeção
 * de shell via SSID de AP rogue — o SSID é interpolado num comando nmcli
 * dentro de wifi_connect_thread, que o quota via g_shell_quote).
 *
 * Estratégia: display real + página Wi-Fi verdadeira (wifi_build_page) +
 * PATH com fakebin à frente (só o nosso nmcli falso responde; o resto do
 * PATH é preservado). O fake LOGA cada argv recebido em $QS_FAKE_LOG.
 * O teste emite "row-activated" na linha da rede adversarial (aberta, sem
 * diálogo de senha) e confere que o comando logado contém o SSID quotado
 * com aspas simples — e que nenhum arquivo marcador foi criado.
 *
 * Compilação: cc ... -o tests/test-wifi-security tests/test-wifi-security.c mais src/ todo ...
 * Execução: ./tests/test-wifi-security (pula sem display).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include "qs-core.h"
#include "qs-net.h"

static gboolean have_display = FALSE;
static gchar *tmp_root = NULL;
static gchar *fake_log = NULL;

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static void
write_fake (void)
{
  gchar *dir = g_build_filename (tmp_root, "fakebin", NULL);
  gchar *fake = g_build_filename (dir, "nmcli", NULL);
  gchar *old_path = g_strdup (g_getenv ("PATH"));
  gchar *new_path = NULL;

  g_mkdir_with_parents (dir, 0755);
  g_file_set_contents (fake,
                       "#!/bin/sh\n"
                       "if [ -n \"${QS_FAKE_LOG:-}\" ]; then\n"
                       "  printf 'nmcli argc=%d cmd=%s\\n' \"$#\" \"$*\" >> \"$QS_FAKE_LOG\"\n"
                       "fi\n"
                       "case \"$*\" in\n"
                       "  \"radio wifi\") echo \"enabled\" ;;\n"
                        "  *\"--rescan yes\"*) printf '%s\\n' \"${QS_FAKE_RESCAN_LINE:-$QS_FAKE_WIFI_LINE}\" ;;\n"
                       "  *\"device wifi list\"*) printf '%s\\n' \"$QS_FAKE_WIFI_LINE\" ;;\n"
                       "esac\n"
                       "exit 0\n",
                       -1, NULL);
  g_chmod (fake, 0755);

  g_rmdir (fake_log);
  g_free (fake_log);
  fake_log = g_build_filename (tmp_root, "argv.log", NULL);

  new_path = g_strdup_printf ("%s:%s", dir, old_path);
  g_setenv ("PATH", new_path, TRUE);
  g_setenv ("QS_FAKE_LOG", fake_log, TRUE);
  g_free (dir);
  g_free (fake);
  g_free (old_path);
  g_free (new_path);
}

static gchar *
read_log (void)
{
  gchar *contents = NULL;

  if (!g_file_get_contents (fake_log, &contents, NULL, NULL))
    return g_strdup ("");
  return contents;
}


/* Polling com timeout (fatias de 100ms): elimina flakes de sleeps fixos
 * e acelera a suite (resolve em ms quando o worker é rápido). */
static void
run_loop_ms (gint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add ((guint) ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static gboolean
log_contains (const gchar *substr)
{
  gchar *log = read_log ();
  gboolean hit = (g_strstr_len (log, -1, substr) != NULL);

  g_free (log);
  return hit;
}

static gboolean
wait_for_log (const gchar *substr, gint timeout_ms)
{
  gint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_contains (substr))
        return TRUE;
      run_loop_ms (100);
      waited += 100;
    }
  return log_contains (substr);
}

/* Acha a row adversarial (prefixo Evil/Caf) — NULL se ausente. */
static GtkListBoxRow *
find_target_row (QuickSettings *qs)
{
  GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
  GList *l = NULL;
  GtkListBoxRow *target = NULL;

  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *ssid = g_object_get_data (G_OBJECT (l->data), "qs-ssid");

      if (ssid != NULL && (g_str_has_prefix (ssid, "Evil")
                           || g_str_has_prefix (ssid, "Caf")))
        target = GTK_LIST_BOX_ROW (l->data);
    }
  g_list_free (rows);
  return target;
}

static GtkListBoxRow *
wait_for_target_row (QuickSettings *qs, gint timeout_ms)
{
  GtkListBoxRow *row = NULL;
  gint waited = 0;

  while (waited < timeout_ms)
    {
      row = find_target_row (qs);
      if (row != NULL)
        return row;
      run_loop_ms (100);
      waited += 100;
    }
  return find_target_row (qs);
}

/* Monta página, carrega lista, ativa a linha do SSID dado, espera o
 * connect completar e devolve o log de argv. */
static gchar *
connect_row (const gchar *ssid_line, const gchar *marker)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  GtkWidget *back = NULL;
  GMainLoop *loop = NULL;
  GtkListBoxRow *target = NULL;
  gchar *log = NULL;

  (void) marker;
  write_fake ();
  g_setenv ("QS_FAKE_WIFI_LINE", ssid_line, TRUE);

  qs->refcount = 1;
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->stack = gtk_stack_new ();
  wifi_build_page (qs->stack, qs, &back);
  /* O mini-tile vive fora da página (fachada qs-net); o fluxo show/update
   * toca nele — widgets reais nunca mapeados bastam. */
  qs->wifi_tile = gtk_button_new ();
  qs->wifi_sub = gtk_label_new ("");
  qs->wifi_switch = gtk_switch_new ();
  wifi_show_page (qs);

  loop = g_main_loop_new (NULL, FALSE);
  target = wait_for_target_row (qs, 10000);
  g_assert_nonnull (target);
  g_signal_emit_by_name (qs->wifi_list, "row-activated", target);

  g_assert_true (wait_for_log ("device wifi connect", 10000));
  /* Settle: dá tempo do idle pós-connect (reload) antes do free. */
  run_loop_ms (1000);
  g_main_loop_unref (loop);

  log = read_log ();
  g_atomic_int_set (&qs->dead, TRUE);
  qs_unref (qs);
  return log;
}

/* SSID com metacaracteres de shell chega ao nmcli dentro de aspas
 * simples — e o payload nunca executa. */
static void
test_connect_quotes_ssid (void)
{
  const gchar *line = "*:Evil\"; touch /tmp/qs-sec-wifi; echo \":80:--";
  gchar *log = NULL;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  g_remove ("/tmp/qs-sec-wifi");
  log = connect_row (line, "/tmp/qs-sec-wifi");
  /* argc=4 prova o quoting: o SSID inteiro chegou como UM argv ao nmcli
   * (sem aspas o shell o quebraria em vários). */
  g_assert_nonnull (g_strstr_len (log, -1, "argc=4 cmd=device wifi connect Evil"));
  g_assert_false (g_file_test ("/tmp/qs-sec-wifi", G_FILE_TEST_EXISTS));
  g_free (log);
}

/* Espaços, $() e backticks também vão quotados. */
static void
test_connect_quotes_specials (void)
{
  const gchar *line = "*:Café $(touch /tmp/qs-sec-wifi2) `id`:70:--";
  gchar *log = NULL;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  g_remove ("/tmp/qs-sec-wifi2");
  log = connect_row (line, "/tmp/qs-sec-wifi2");
  g_assert_nonnull (g_strstr_len (log, -1, "argc=4 cmd=device wifi connect Caf"));
  g_assert_false (g_file_test ("/tmp/qs-sec-wifi2", G_FILE_TEST_EXISTS));
  g_free (log);
}

/* Scan emite o comando fixo de rescan (sem interpolação). */
static void
test_rescan_fixed_command (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  GtkWidget *back = NULL;
  GMainLoop *loop = NULL;
  gchar *log = NULL;

  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }
  write_fake ();
  g_setenv ("QS_FAKE_WIFI_LINE", "*:Home:80:WPA2", TRUE);
  g_setenv ("QS_FAKE_RESCAN_LINE", "*:RescanNet:70:WPA2", TRUE);
  qs->refcount = 1;
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->stack = gtk_stack_new ();
  wifi_build_page (qs->stack, qs, &back);
  qs->wifi_tile = gtk_button_new ();
  qs->wifi_sub = gtk_label_new ("");
  qs->wifi_switch = gtk_switch_new ();
  g_signal_emit_by_name (qs->wifi_scan, "clicked");
  loop = g_main_loop_new (NULL, FALSE);
  g_assert_true (wait_for_log ("device wifi list --rescan yes", 10000));
  run_loop_ms (500);
  g_main_loop_unref (loop);
  log = read_log ();
  g_assert_nonnull (g_strstr_len (log, -1, "device wifi list --rescan yes"));
  g_free (log);
  {
    GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
    GList *l = NULL;
    gboolean found = FALSE;

    for (l = rows; l != NULL; l = l->next)
      {
        const gchar *ssid = g_object_get_data (G_OBJECT (l->data), "qs-ssid");

        if (g_strcmp0 (ssid, "RescanNet") == 0)
          found = TRUE;
      }
    g_list_free (rows);
    g_assert_true (found);
  }
  g_atomic_int_set (&qs->dead, TRUE);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-wifisec-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/wifi-security/connect_quotes_ssid", test_connect_quotes_ssid);
  g_test_add_func ("/wifi-security/connect_quotes_specials", test_connect_quotes_specials);
  g_test_add_func ("/wifi-security/rescan_fixed_command", test_rescan_fixed_command);

  return g_test_run ();
}
