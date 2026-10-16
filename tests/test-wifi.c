/* Testes do grupo 3: qs-wifi-backend (parse puro + fake nmcli via PATH). */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "qs-wifi-backend.h"

static gchar *saved_path = NULL;

static void
push_fakebin (void)
{
  if (saved_path == NULL)
    saved_path = g_strdup (g_getenv ("PATH"));
  gchar *np = g_strconcat ("tests/fakebin:", saved_path, NULL);

  g_setenv ("PATH", np, TRUE);
  g_free (np);
}

static void
pop_path (void)
{
  g_setenv ("PATH", saved_path, TRUE);
}

static void
test_icon (void)
{
  g_assert_cmpstr (wifi_signal_icon (100), ==, "network-wireless-signal-excellent-symbolic");
  g_assert_cmpstr (wifi_signal_icon (75), ==, "network-wireless-signal-excellent-symbolic");
  g_assert_cmpstr (wifi_signal_icon (74), ==, "network-wireless-signal-good-symbolic");
  g_assert_cmpstr (wifi_signal_icon (50), ==, "network-wireless-signal-good-symbolic");
  g_assert_cmpstr (wifi_signal_icon (49), ==, "network-wireless-signal-ok-symbolic");
  g_assert_cmpstr (wifi_signal_icon (25), ==, "network-wireless-signal-ok-symbolic");
  g_assert_cmpstr (wifi_signal_icon (24), ==, "network-wireless-signal-weak-symbolic");
  g_assert_cmpstr (wifi_signal_icon (0), ==, "network-wireless-signal-weak-symbolic");
  g_assert_cmpstr (wifi_signal_icon (-5), ==, "network-wireless-signal-weak-symbolic");
}

static void
test_parse_basic (void)
{
  const gchar *out = "*:HomeNet:80:WPA2\n:Cafe:45:WPA1 WPA2\n:OpenNet::--\n";
  GList       *nets = parse_wifi_list (out);
  WifiNet     *a, *b, *c;

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 3);
  a = nets->data;
  b = nets->next->data;
  c = nets->next->next->data;
  g_assert_cmpstr (a->ssid, ==, "HomeNet");
  g_assert_cmpint (a->signal, ==, 80);
  g_assert_true (a->secured && a->active);
  g_assert_cmpstr (b->ssid, ==, "Cafe");
  g_assert_cmpint (b->signal, ==, 45);
  g_assert_true (b->secured && !b->active);
  g_assert_cmpstr (c->ssid, ==, "OpenNet");
  g_assert_cmpint (c->signal, ==, 0);
  g_assert_true (!c->secured && !c->active);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
}

static void
test_parse_dedup (void)
{
  /* mesmo SSID duas vezes: fica o maior sinal; active se algum '*' */
  GList   *nets = parse_wifi_list ("*:Net:50:WPA2\n:Net:80:WPA2\n");
  WifiNet *n;

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 1);
  n = nets->data;
  g_assert_cmpint (n->signal, ==, 80);
  g_assert_true (n->active);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
}

static void
test_parse_skip (void)
{
  GList *nets = parse_wifi_list ("\n\n*:A:10:WPA2\n*::80:WPA2\n:B:20:WPA2\n");

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 2);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
  g_assert_null (parse_wifi_list (NULL));
}

static void
test_enabled_sync (void)
{
  push_fakebin ();
  g_setenv ("QS_FAKE_WIFI_STATE", "enabled", TRUE);
  g_assert_true (wifi_enabled_sync ());
  g_setenv ("QS_FAKE_WIFI_STATE", "disabled", TRUE);
  g_assert_false (wifi_enabled_sync ());
  g_unsetenv ("QS_FAKE_WIFI_STATE");
  pop_path ();
}

static void
test_available (void)
{
  push_fakebin ();
  g_assert_true (wifi_available ());
  pop_path ();
  g_setenv ("PATH", "/tmp/no-dir-xyz-123", TRUE);
  g_assert_false (wifi_available ());
  pop_path ();
}

static void
test_set_enabled (void)
{
  const gchar *log = "/tmp/qs-test-nmcli.log";

  push_fakebin ();
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_remove (log);
  /* spawn é assíncrono: espera até ~2s pelo log do fake. */
  wifi_set_enabled (TRUE);
  gboolean seen = FALSE;

  for (gint i = 0; i < 40 && !seen; i++)
    {
      gchar *content = NULL;

      g_usleep (50000);
      if (g_file_get_contents (log, &content, NULL, NULL) && content != NULL)
        seen = strstr (content, "radio wifi on") != NULL;
      g_free (content);
    }
  g_assert_true (seen);
  g_unsetenv ("QS_FAKE_LOG");
  g_remove (log);
  pop_path ();
}

gint
main (gint argc, gchar **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/wifi/icon", test_icon);
  g_test_add_func ("/wifi/parse/basic", test_parse_basic);
  g_test_add_func ("/wifi/parse/dedup", test_parse_dedup);
  g_test_add_func ("/wifi/parse/skip", test_parse_skip);
  g_test_add_func ("/wifi/enabled_sync", test_enabled_sync);
  g_test_add_func ("/wifi/available", test_available);
  g_test_add_func ("/wifi/set_enabled", test_set_enabled);
  return g_test_run ();
}
