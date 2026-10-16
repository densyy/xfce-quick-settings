/* Corpus adversarial + entradas gigantes nos parsers públicos (fuzz manual
 * determinístico — sem libfuzzer; cada caso é um assert GTest).
 *
 * Alvos: parse_wifi_list, wifi_signal_icon, bt_output_is_error,
 * unescape_mount, sysmon_bytes_text, qs_parse_int_checked, split_terse_line,
 * parse_first_percent, bright_extract_output, valid_monitor_shared,
 * qs_display_snap_new, flat_text, volume_icon_for_level + sqlite maligno.
 * Objetivo: nenhum crash, nenhum hang, nenhum resultado absurdo
 * (ex.: fraction fora de [0,1], índice negativo) com entrada hostil.
 *
 * Compilação: cc ... -o tests/test-fuzz tests/test-fuzz.c mais src/ todo ...
 * Execução: ./tests/test-fuzz (sem display, sem rede, sem DB real).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "qs-util.h"
#include "qs-wifi-backend.h"
#include "qs-bt-backend.h"
#include "qs-sysmon-data.h"
#include "qs-monitor-data.h"
#include "qs-display.h"
#include "qs-brightness.h"
#include "qs-audio-backend.h"
#include "qs-notify-store.h"

static gchar *tmp_root = NULL;

/* SSIDs hostis passam verbatim pelo parser sem quebrar a estrutura. */
static void
test_wifi_evil_ssids (void)
{
  const gchar *terse =
    "*:Evil\"; touch /tmp/pwned; echo \":80:WPA2\n"
    "*:Café $(id) `id`:70:WPA2\n"
    "*:a:b:c:d:60:WPA2\n"
    "*::50:WPA2\n"
    "*:  :40:--\n"
    ":Normal:30:WPA2\n";
  GList *nets = parse_wifi_list (terse);
  gint n = 0;
  GList *l = NULL;

  g_assert_nonnull (nets);
  for (l = nets; l != NULL; l = l->next)
    {
      WifiNet *wn = l->data;

      n++;
      g_assert_nonnull (wn->ssid);
      g_assert_cmpint (wn->signal, >=, 0);
    }
  /* Linha SSID-vazio é pulada; as demais (5) sobrevivem. */
  g_assert_cmpint (n, ==, 5);
  g_list_free_full (nets, wifi_net_free);
}

/* 5000 redes: sem hang, sem perda. */
static void
test_wifi_giant_list (void)
{
  GString *terse = g_string_new (NULL);
  GList *nets = NULL;
  gint i = 0;

  for (i = 0; i < 5000; i++)
    g_string_append_printf (terse, " :Net%04d:%d:WPA2\n", i, i % 100);
  nets = parse_wifi_list (terse->str);
  g_assert_cmpint (g_list_length (nets), ==, 5000);
  g_list_free_full (nets, wifi_net_free);
  g_string_free (terse, TRUE);
}

/* Ícones com sinais extremos: sempre um nome válido. */
static void
test_wifi_signal_bounds (void)
{
  g_assert_nonnull (wifi_signal_icon (G_MAXINT));
  g_assert_nonnull (wifi_signal_icon (G_MININT));
  g_assert_nonnull (wifi_signal_icon (-1));
  g_assert_nonnull (wifi_signal_icon (101));
}

/* Saídas hostis do bluetoothctl: sem crash, classificação sã. */
static void
test_bt_evil_output (void)
{
  g_assert_false (bt_output_is_error ("CONNECTION SUCCESSFUL", ""));
  g_assert_true (bt_output_is_error ("FAILED", ""));
  g_assert_true (bt_output_is_error ("failed", ""));
  g_assert_false (bt_output_is_error ("", ""));
  g_assert_false (bt_output_is_error ("Failed to check AlreadyExists state", ""));
  g_assert_true (bt_output_is_error ("org.bluez.Error.AuthenticationFailed", ""));
}

/* Escapes truncados e lixo: nunca crasha, resto passa literal. */
static void
test_unescape_evil (void)
{
  gchar *r1 = unescape_mount ("trailing\\");
  gchar *r2 = unescape_mount ("\\1");
  gchar *r3 = unescape_mount ("\\999");
  gchar *r4 = unescape_mount ("\\040\\040\\040");
  gchar *r5 = unescape_mount ("\\134\\134");

  g_assert_cmpstr (r1, ==, "trailing\\");
  g_assert_cmpstr (r2, ==, "\\1");
  g_assert_cmpstr (r3, ==, "\\999");
  g_assert_cmpstr (r4, ==, "   ");
  g_assert_cmpstr (r5, ==, "\\\\");
  g_free (r1);
  g_free (r2);
  g_free (r3);
  g_free (r4);
  g_free (r5);
}

/* 1MB de escapes: sem hang. */
static void
test_unescape_giant (void)
{
  GString *in = g_string_new (NULL);
  gchar *out = NULL;
  gint i = 0;

  for (i = 0; i < 250000; i++)
    g_string_append (in, "\\040");
  out = unescape_mount (in->str);
  g_assert_cmpuint (strlen (out), ==, 250000);
  g_free (out);
  g_string_free (in, TRUE);
}

/* Inteiros hostis: hex, unicode, dígitos gigantes. */
static void
test_parse_int_evil (void)
{
  gint v = -1;

  g_assert_false (qs_parse_int_checked ("0x10", &v));
  g_assert_true (qs_parse_int_checked ("  +00042  ", &v));
  g_assert_cmpint (v, ==, 42);
  /* Dígitos unicode de largura total não são ASCII → rejeitado. */
  g_assert_false (qs_parse_int_checked ("１２３", &v));
  {
    gchar *huge = g_strnfill (100000, '9');

    g_assert_false (qs_parse_int_checked (huge, &v));
    g_free (huge);
  }
  g_assert_false (qs_parse_int_checked ("99999999999999999999999", &v));
  g_assert_false (qs_parse_int_checked ("-", &v));
  g_assert_false (qs_parse_int_checked ("+", &v));
}

/* Split hostil: 10KB por campo, backslash final. */
static void
test_split_evil (void)
{
  gchar *big = g_strnfill (10000, 'A');
  gchar *line = g_strdup_printf ("%s:%s:5:WPA2", big, big);
  gchar **f = split_terse_line (line, 4);

  g_assert_cmpuint (strlen (f[0]), ==, 10000);
  g_assert_cmpuint (strlen (f[1]), ==, 10000);
  g_assert_cmpstr (f[2], ==, "5");
  g_assert_cmpstr (f[3], ==, "WPA2");
  g_strfreev (f);
  g_free (line);
  g_free (big);
}

/* Percent hostil. */
static void
test_percent_evil (void)
{
  g_assert_cmpint (parse_first_percent ("Vol: %"), ==, -1);
  g_assert_cmpint (parse_first_percent ("100% 0%"), ==, 100);
  g_assert_cmpint (parse_first_percent ("99999999999999999999%"), ==, -1);
  g_assert_cmpint (parse_first_percent ("-5%"), ==, 5);
}

/* xrandr hostil: linha gigante, \"connected\" em posição estranha. */
static void
test_bright_evil (void)
{
  gchar *big = g_strnfill (10000, 'X');
  gchar *dump = g_strdup_printf ("%s connected primary 1920x1080+0+0\n", big);
  gchar *out = bright_extract_output (dump);

  /* Nome >64 chars é inválido → NULL, sem crash. */
  g_assert_null (out);
  g_free (dump);
  g_free (big);
  g_assert_null (bright_extract_output ("HDMI-1 disconnected\n"));
  g_assert_null (bright_extract_output ("totally-connectedish\n"));
}

/* Monitor/snap hostil. */
static void
test_monitor_evil (void)
{
  gchar *big = g_strnfill (10000, 'M');
  gchar *dump = g_strdup_printf ("%s connected 1920x1080+0+0\n", big);
  QsDisplaySnap *snap = qs_display_snap_new (dump);

  /* Nome gigante inválido → nenhum monitor. */
  g_assert_cmpuint (snap->n_connected, ==, 0);
  qs_display_snap_free (snap);
  g_free (dump);
  g_free (big);
  g_assert_false (valid_monitor_shared ("a b"));
  g_assert_false (valid_monitor_shared ("a/b"));
  g_assert_true (valid_mode ("9999999999x9999999999"));
  {
    guint i = 0;

    for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
      g_assert_true (valid_scale (SCALE_FACTORS[i]));
  }
}

/* flat_text hostil: 1MB + só delimitadores. */
static void
test_flat_evil (void)
{
  gchar *big = g_strnfill (1000000, '\n');
  gchar *flat = flat_text ((const guchar *) big);

  g_assert_cmpuint (strlen (flat), ==, 1000000);
  g_free (flat);
  g_free (big);
}

/* Ícones de volume com extremos. */
static void
test_volume_bounds (void)
{
  g_assert_nonnull (volume_icon_for_level (G_MAXINT));
  g_assert_nonnull (volume_icon_for_level (G_MININT));
}

/* SQLite maligno: strings de 1MB + tipo errado na coluna timestamp. */
static void
test_sqlite_evil (void)
{
  gchar *dir = g_build_filename (tmp_root, "xfce4", "notifyd", NULL);
  gchar *path = g_build_filename (dir, "log.sqlite", NULL);
  gchar *huge = g_strnfill (1000000, 'B');
  sqlite3 *db = NULL;
  GList *notices = NULL;

  g_mkdir_with_parents (dir, 0700);
  g_remove (path);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_exec (db, "CREATE TABLE notifications(app_name TEXT, summary TEXT, body TEXT, timestamp INTEGER);",
                NULL, NULL, NULL);
  {
    gchar *sql = g_strdup_printf ("INSERT INTO notifications VALUES('A', 'S', '%s', 'notanumber');", huge);

    sqlite3_exec (db, sql, NULL, NULL, NULL);
    g_free (sql);
  }
  sqlite3_close (db);
  notices = list_notices ();
  g_assert_cmpint (g_list_length (notices), ==, 1);
  g_list_free_full (notices, notice_free);
  g_free (huge);
  g_free (dir);
  g_free (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-fuzz-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);

  g_test_add_func ("/fuzz/wifi_evil_ssids", test_wifi_evil_ssids);
  g_test_add_func ("/fuzz/wifi_giant_list", test_wifi_giant_list);
  g_test_add_func ("/fuzz/wifi_signal_bounds", test_wifi_signal_bounds);
  g_test_add_func ("/fuzz/bt_evil_output", test_bt_evil_output);
  g_test_add_func ("/fuzz/unescape_evil", test_unescape_evil);
  g_test_add_func ("/fuzz/unescape_giant", test_unescape_giant);
  g_test_add_func ("/fuzz/parse_int_evil", test_parse_int_evil);
  g_test_add_func ("/fuzz/split_evil", test_split_evil);
  g_test_add_func ("/fuzz/percent_evil", test_percent_evil);
  g_test_add_func ("/fuzz/bright_evil", test_bright_evil);
  g_test_add_func ("/fuzz/monitor_evil", test_monitor_evil);
  g_test_add_func ("/fuzz/flat_evil", test_flat_evil);
  g_test_add_func ("/fuzz/volume_bounds", test_volume_bounds);
  g_test_add_func ("/fuzz/sqlite_evil", test_sqlite_evil);

  return g_test_run ();
}
