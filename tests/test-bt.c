/* Testes do grupo 4: qs-bt-backend (helpers puros + fake bluetoothctl via PATH). */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "qs-bt-backend.h"

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
test_contains_ci (void)
{
  g_assert_true (bt_str_contains_ci ("Hello World", "hello"));
  g_assert_true (bt_str_contains_ci ("Hi", ""));
  g_assert_false (bt_str_contains_ci ("Hi", "bye"));
  g_assert_false (bt_str_contains_ci (NULL, "x"));
  g_assert_false (bt_str_contains_ci ("x", NULL));
}

static void
test_output_is_error (void)
{
  g_assert_false (bt_output_is_error ("Connection successful", ""));
  g_assert_false (bt_output_is_error ("Failed to pair: AlreadyExists", ""));
  g_assert_false (bt_output_is_error (NULL, NULL));
  g_assert_false (bt_output_is_error ("some random text", ""));
  g_assert_true (bt_output_is_error ("Failed to connect: blah", ""));
  g_assert_true (bt_output_is_error ("", "Not available"));
  g_assert_true (bt_output_is_error ("", "No agent available"));
  g_assert_true (bt_output_is_error ("Authentication failed", ""));
  g_assert_true (bt_output_is_error ("org.bluez.Error.AuthenticationFailed", ""));
}

static void
test_controller (void)
{
  gchar *c;

  push_fakebin ();
  c = bt_get_controller ();
  g_assert_cmpstr (c, ==, "AA:BB:CC:DD:EE:FF");
  g_free (c);
  pop_path ();
}

static void
test_powered (void)
{
  push_fakebin ();
  g_assert_true (bt_powered ("AA:BB:CC:DD:EE:FF"));
  g_assert_false (bt_powered (NULL));
  pop_path ();
}

static void
test_fill_flags (void)
{
  BtDevice dev = {
    g_strdup ("AA:BB:CC:DD:EE:FF"), g_strdup ("FakeBuds"),
    FALSE, FALSE, FALSE
  };

  push_fakebin ();
  bt_fill_flags (&dev);
  g_assert_true (dev.paired && !dev.trusted && dev.connected);
  g_free (dev.mac);
  g_free (dev.name);
  pop_path ();
}

static BtDevice *
find_dev (GList *devs, const gchar *mac)
{
  for (GList *l = devs; l != NULL; l = l->next)
    {
      BtDevice *d = l->data;

      if (g_strcmp0 (d->mac, mac) == 0)
        return d;
    }
  return NULL;
}

static void
test_list (void)
{
  GList    *devs;
  BtDevice *kb, *mouse, *hs;

  push_fakebin ();
  devs = list_bt_devices ();
  g_assert_nonnull (devs);
  g_assert_cmpuint (g_list_length (devs), ==, 3);
  kb = find_dev (devs, "11:22:33:44:55:66");
  mouse = find_dev (devs, "AA:AA:AA:AA:AA:01");
  hs = find_dev (devs, "BB:BB:BB:BB:BB:02");
  g_assert_nonnull (kb);
  g_assert_nonnull (mouse);
  g_assert_nonnull (hs);
  g_assert_cmpstr (kb->name, ==, "Keyboard");
  g_assert_true (kb->paired && !kb->trusted && !kb->connected);
  g_assert_true (!mouse->paired && mouse->trusted && !mouse->connected);
  g_assert_true (!hs->paired && !hs->trusted && hs->connected);
  g_list_free_full (devs, bt_device_free);
  pop_path ();
}

static void
test_set_powered (void)
{
  const gchar *log = "/tmp/qs-test-bt.log";
  gboolean     seen = FALSE;
  gint         i;

  push_fakebin ();
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_remove (log);
  /* spawn é assíncrono: espera até ~2s pelo log do fake. */
  bt_set_powered (TRUE);
  for (i = 0; i < 40 && !seen; i++)
    {
      gchar *content = NULL;

      g_usleep (50000);
      if (g_file_get_contents (log, &content, NULL, NULL) && content != NULL)
        seen = strstr (content, "power on") != NULL;
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
  g_test_add_func ("/bt/contains_ci", test_contains_ci);
  g_test_add_func ("/bt/output_is_error", test_output_is_error);
  g_test_add_func ("/bt/controller", test_controller);
  g_test_add_func ("/bt/powered", test_powered);
  g_test_add_func ("/bt/fill_flags", test_fill_flags);
  g_test_add_func ("/bt/list", test_list);
  g_test_add_func ("/bt/set_powered", test_set_powered);
  return g_test_run ();
}
