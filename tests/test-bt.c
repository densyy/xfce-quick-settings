/* Testes do grupo 4: qs-bt-backend (helpers puros + fake bluetoothctl via PATH). */
#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "qs-bt-agent.h"
#include "qs-bt-backend.h"
#include "qs-bt-signal.h"
#include "qs-util.h"

#include "bt-signal-helper.h"

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

  /* Furos sem nome (base + listados filtrados): só o válido sobrevive. */
  push_fakebin ();
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
  devs = list_bt_devices ();
  g_assert_nonnull (devs);
  g_assert_cmpuint (g_list_length (devs), ==, 1);
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:11"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:12"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:13"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:14"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:15"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:16"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:17"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:18"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:21"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:22"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:23"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:24"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:25"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:26"));
  kb = find_dev (devs, "11:22:33:44:55:66");
  g_assert_nonnull (kb);
  g_assert_cmpstr (kb->name, ==, "Buds");
  g_list_free_full (devs, bt_device_free);
  g_unsetenv ("QS_FAKE_BT_DEVICES");
  g_unsetenv ("QS_FAKE_BT_PAIRED");
  g_unsetenv ("QS_FAKE_BT_TRUSTED");
  g_unsetenv ("QS_FAKE_BT_CONNECTED");
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

static GVariant *
make_connected_dict (gboolean connected)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "Connected",
                         g_variant_new_boolean (connected));
  return g_variant_builder_end (&builder);
}

static GVariant *
make_empty_dict (void)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  return g_variant_builder_end (&builder);
}

static GVariant *
make_string_dict (void)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "Connected",
                         g_variant_new_string ("yes"));
  return g_variant_builder_end (&builder);
}

static void
test_signal_exact_true (void)
{
  GVariant *dict = make_connected_dict (TRUE);
  BtSignalMatch m = bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict);

  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_TRUE);
  g_variant_unref (dict);
}

static void
test_signal_exact_false (void)
{
  GVariant *dict = make_connected_dict (FALSE);
  BtSignalMatch m = bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict);

  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_FALSE);
  g_variant_unref (dict);
}

static void
test_signal_case_insensitive (void)
{
  GVariant *dict = make_connected_dict (TRUE);
  BtSignalMatch m = bt_signal_event_matches (
    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
    "aa:bb:cc:dd:ee:ff", dict);

  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_TRUE);
  g_variant_unref (dict);
  dict = make_connected_dict (FALSE);
  m = bt_signal_event_matches (
    "/org/bluez/hci0/dev_aa_bb_cc_dd_ee_ff",
    "AA:BB:CC:DD:EE:FF", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_FALSE);
  g_variant_unref (dict);
}

static void
test_signal_hci_prefix (void)
{
  GVariant *dict = make_connected_dict (TRUE);
  BtSignalMatch m = bt_signal_event_matches (
    "/org/bluez/hci1/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict);

  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_TRUE);
  g_variant_unref (dict);
  dict = make_connected_dict (FALSE);
  m = bt_signal_event_matches (
    "/org/bluez/hci10/dev_BB_BB_BB_BB_BB_02",
    "BB:BB:BB:BB:BB:02", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_FALSE);
  g_variant_unref (dict);
}

static void
test_signal_other_device (void)
{
  GVariant *dict = make_connected_dict (TRUE);
  BtSignalMatch m = bt_signal_event_matches (
    "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_01",
    "11:22:33:44:55:66", dict);

  g_assert_cmpint (m, ==, BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static void
test_signal_strange_path (void)
{
  GVariant *dict = make_connected_dict (TRUE);

  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/other_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict), ==, BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "/weird/path", "11:22:33:44:55:66", dict), ==,
    BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "", "11:22:33:44:55:66", dict), ==, BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_", "11:22:33:44:55:66", dict), ==,
    BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static void
test_signal_no_connected (void)
{
  GVariant *dict = make_empty_dict ();

  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict), ==, BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static void
test_signal_wrong_type (void)
{
  GVariant *dict = make_string_dict ();

  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", dict), ==, BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static void
test_signal_nulls (void)
{
  GVariant *dict = make_connected_dict (TRUE);

  g_assert_cmpint (bt_signal_event_matches (
    NULL, "11:22:33:44:55:66", dict), ==, BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66", NULL, dict), ==,
    BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66",
    "11:22:33:44:55:66", NULL), ==, BT_SIGNAL_NO_MATCH);
  g_assert_cmpint (bt_signal_event_matches (
    "/org/bluez/hci0/dev_11_22_33_44_55_66", "", dict), ==,
    BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static void
test_dash_mac_filter (void)
{
  GList    *devs;
  BtDevice *hit;

  push_fakebin ();
  g_setenv ("QS_FAKE_BT_DEVICES",
            "Device 63:2F:6B:7A:0D:E9 63-2F-6B-7A-0D-E9\n"
            "Device 10:5A:17:13:52:05 10-5A-17-13-52-05\n"
            "Device AA:BB:CC:DD:EE:12 AABBCCDDEE12\n"
            "Device AA:BB:CC:DD:EE:13 aa-bb-cc-dd-ee-13\n"
            "Device AA:BB:CC:DD:EE:14 My-Headset-Pro\n"
            "Device AA:BB:CC:DD:EE:15 AABBCCDDEE15\n"
            "Device 11:22:33:44:55:66 Buds",
            TRUE);
  g_setenv ("QS_FAKE_BT_PAIRED", "", TRUE);
  g_setenv ("QS_FAKE_BT_TRUSTED", "", TRUE);
  g_setenv ("QS_FAKE_BT_CONNECTED", "", TRUE);
  devs = list_bt_devices ();
  g_assert_nonnull (devs);
  g_assert_null (find_dev (devs, "63:2F:6B:7A:0D:E9"));
  g_assert_null (find_dev (devs, "10:5A:17:13:52:05"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:12"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:13"));
  g_assert_null (find_dev (devs, "AA:BB:CC:DD:EE:15"));
  hit = find_dev (devs, "AA:BB:CC:DD:EE:14");
  g_assert_nonnull (hit);
  g_assert_cmpstr (hit->name, ==, "My-Headset-Pro");
  hit = find_dev (devs, "11:22:33:44:55:66");
  g_assert_nonnull (hit);
  g_assert_cmpstr (hit->name, ==, "Buds");
  g_assert_cmpuint (g_list_length (devs), ==, 2);
  g_list_free_full (devs, bt_device_free);
  g_unsetenv ("QS_FAKE_BT_DEVICES");
  g_unsetenv ("QS_FAKE_BT_PAIRED");
  g_unsetenv ("QS_FAKE_BT_TRUSTED");
  g_unsetenv ("QS_FAKE_BT_CONNECTED");
  pop_path ();
}

static void
test_pair_fail (void)
{
  gchar *output;

  push_fakebin ();
  output = run_command ("bluetoothctl --timeout 1 pair 11:22:33:44:55:66");
  g_assert_nonnull (output);
  g_assert_false (bt_output_is_error (output, ""));
  g_assert_true (strstr (output, "Pairing successful") != NULL);
  g_free (output);

  g_setenv ("QS_FAKE_BT_PAIR_FAIL", "1", TRUE);
  output = run_command ("bluetoothctl --timeout 1 pair 11:22:33:44:55:66");
  g_assert_nonnull (output);
  g_assert_true (bt_output_is_error (output, ""));
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_PAIR_FAIL");

  /* Em stderr o run_command descarta o texto: prova pelo log do verbo. */
  g_setenv ("QS_FAKE_BT_PAIR_FAIL", "2", TRUE);
  output = run_command ("bluetoothctl --timeout 1 pair 11:22:33:44:55:66");
  g_assert_true (output == NULL || strstr (output, "successful") == NULL);
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_PAIR_FAIL");
  pop_path ();
}

static void
test_trust_verb (void)
{
  const gchar *log = "/tmp/qs-test-bt-trust.log";
  gchar       *output;
  gchar       *text = NULL;

  push_fakebin ();
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_remove (log);
  output = run_command ("bluetoothctl --timeout 1 trust 11:22:33:44:55:66");
  g_assert_nonnull (output);
  g_assert_false (bt_output_is_error (output, ""));
  g_assert_true (strstr (output, "trust succeeded") != NULL);
  g_free (output);
  g_assert_true (g_file_get_contents (log, &text, NULL, NULL));
  g_assert_true (strstr (text, "trust 11:22:33:44:55:66") != NULL);
  g_free (text);
  text = NULL;

  g_setenv ("QS_FAKE_BT_TRUST_FAIL", "1", TRUE);
  output = run_command ("bluetoothctl --timeout 1 trust 11:22:33:44:55:66");
  g_assert_nonnull (output);
  g_assert_true (bt_output_is_error (output, ""));
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_TRUST_FAIL");

  g_setenv ("QS_FAKE_BT_TRUST_FAIL", "2", TRUE);
  output = run_command ("bluetoothctl --timeout 1 trust 11:22:33:44:55:66");
  g_assert_true (output == NULL || strstr (output, "succeeded") == NULL);
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_TRUST_FAIL");

  g_unsetenv ("QS_FAKE_LOG");
  g_remove (log);
  pop_path ();
}

static void
test_alias_verb (void)
{
  const gchar *log = "/tmp/qs-test-bt-alias.log";
  gchar       *output;
  gchar       *text = NULL;

  push_fakebin ();
  g_setenv ("QS_FAKE_LOG", log, TRUE);
  g_remove (log);
  output = run_command (
    "bluetoothctl --timeout 5 set-alias 11:22:33:44:55:66 NewName");
  g_assert_nonnull (output);
  g_assert_false (bt_output_is_error (output, ""));
  g_assert_true (strstr (output, "alias succeeded") != NULL);
  g_free (output);
  g_assert_true (g_file_get_contents (log, &text, NULL, NULL));
  g_assert_true (strstr (text, "set-alias 11:22:33:44:55:66") != NULL);
  g_free (text);
  text = NULL;

  g_setenv ("QS_FAKE_BT_ALIAS_FAIL", "1", TRUE);
  output = run_command (
    "bluetoothctl --timeout 5 set-alias 11:22:33:44:55:66 NewName");
  g_assert_nonnull (output);
  g_assert_true (bt_output_is_error (output, ""));
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_ALIAS_FAIL");

  g_setenv ("QS_FAKE_BT_ALIAS_FAIL", "2", TRUE);
  output = run_command (
    "bluetoothctl --timeout 5 set-alias 11:22:33:44:55:66 NewName");
  g_assert_true (output == NULL || strstr (output, "succeeded") == NULL);
  g_free (output);
  g_unsetenv ("QS_FAKE_BT_ALIAS_FAIL");

  g_unsetenv ("QS_FAKE_LOG");
  g_remove (log);
  pop_path ();
}

static void
test_page_dict_match (void)
{
  GVariant *dict = bt_helper_page_dict (TRUE, TRUE);
  BtSignalMatch m;

  /* A chave extra Paired nao quebra o casamento de Connected. */
  m = bt_signal_event_matches ("/org/bluez/hci0/dev_11_22_33_44_55_66",
                               "11:22:33:44:55:66", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_TRUE);
  g_variant_unref (dict);
  dict = bt_helper_page_dict (TRUE, FALSE);
  m = bt_signal_event_matches ("/org/bluez/hci0/dev_11_22_33_44_55_66",
                               "11:22:33:44:55:66", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_FALSE);
  g_variant_unref (dict);
  dict = bt_helper_page_dict (FALSE, TRUE);
  m = bt_signal_event_matches ("/org/bluez/hci1/dev_11_22_33_44_55_66",
                               "11:22:33:44:55:66", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_CONNECTED_TRUE);
  g_variant_unref (dict);
  dict = bt_helper_page_dict (TRUE, TRUE);
  m = bt_signal_event_matches ("/org/bluez/hci0/dev_AA_AA_AA_AA_AA_01",
                               "11:22:33:44:55:66", dict);
  g_assert_cmpint (m, ==, BT_SIGNAL_NO_MATCH);
  g_variant_unref (dict);
}

static gchar *emit_last_path = NULL;
static gboolean emit_got_paired = FALSE;
static gboolean emit_got_connected = FALSE;
static gboolean emit_called = FALSE;

static gboolean
lookup_bool (GVariant    *dict,
             const gchar *key,
             gboolean    *out)
{
  GVariant *val;
  gboolean  ok = FALSE;

  if (dict == NULL || key == NULL || out == NULL)
    return FALSE;
  val = g_variant_lookup_value (dict, key, NULL);
  if (val == NULL)
    return FALSE;
  if (g_variant_is_of_type (val, G_VARIANT_TYPE_VARIANT))
    {
      GVariant *inner = g_variant_get_variant (val);

      if (g_variant_is_of_type (inner, G_VARIANT_TYPE_BOOLEAN))
        {
          *out = g_variant_get_boolean (inner);
          ok = TRUE;
        }
      g_variant_unref (inner);
    }
  else if (g_variant_is_of_type (val, G_VARIANT_TYPE_BOOLEAN))
    {
      *out = g_variant_get_boolean (val);
      ok = TRUE;
    }
  g_variant_unref (val);
  return ok;
}

static void
page_emit_cb (const gchar *object_path,
              GVariant    *changed,
              const gchar *sender_name,
              gpointer     user_data)
{
  gboolean v = FALSE;

  (void) sender_name;
  (void) user_data;
  emit_called = TRUE;
  g_free (emit_last_path);
  emit_last_path = g_strdup (object_path);
  if (lookup_bool (changed, "Paired", &v))
    emit_got_paired = v;
  if (lookup_bool (changed, "Connected", &v))
    emit_got_connected = v;
}

static void
test_page_properties_emit (void)
{
  BtSignalWatch *watch;

  watch = bt_signal_watch_new_for_test (page_emit_cb, NULL);
  g_assert_nonnull (watch);
  emit_called = FALSE;
  emit_got_paired = FALSE;
  emit_got_connected = FALSE;
  g_free (emit_last_path);
  emit_last_path = NULL;
  bt_helper_emit_page_properties (watch, "11:22:33:44:55:66", TRUE, TRUE);
  g_assert_true (emit_called);
  g_assert_cmpstr (emit_last_path, ==,
                   "/org/bluez/hci0/dev_11_22_33_44_55_66");
  g_assert_true (emit_got_paired);
  g_assert_true (emit_got_connected);
  bt_signal_unsubscribe (watch);

  emit_called = FALSE;
  emit_got_paired = TRUE;
  emit_got_connected = TRUE;
  watch = bt_signal_watch_new_for_test (page_emit_cb, NULL);
  g_assert_nonnull (watch);
  bt_helper_emit_page_properties (watch, "11:22:33:44:55:66", FALSE, FALSE);
  g_assert_true (emit_called);
  g_assert_false (emit_got_paired);
  g_assert_false (emit_got_connected);
  bt_signal_unsubscribe (watch);
  g_free (emit_last_path);
  emit_last_path = NULL;

  /* NULL-safe: nada acontece e nada aborta. */
  bt_helper_emit_page_properties (NULL, "11:22:33:44:55:66", TRUE, TRUE);
  watch = bt_signal_watch_new_for_test (page_emit_cb, NULL);
  g_assert_nonnull (watch);
  bt_helper_emit_page_properties (watch, NULL, TRUE, TRUE);
  bt_signal_unsubscribe (watch);
}

static void
test_trust_helpers (void)
{
  gchar *cmd;

  g_assert_true (bt_mac_is_valid ("11:22:33:44:55:66"));
  g_assert_false (bt_mac_is_valid (NULL));
  g_assert_false (bt_mac_is_valid ("curto"));
  g_assert_false (bt_mac_is_valid ("11:22:33:44:55:6G"));
  g_assert_false (bt_mac_is_valid ("11-22-33-44-55-66"));
  cmd = bt_build_trust_command ("11:22:33:44:55:66");
  g_assert_nonnull (cmd);
  g_assert_true (strstr (cmd, "trust") != NULL);
  g_assert_true (strstr (cmd, "--timeout 5") != NULL);
  g_free (cmd);
  g_assert_null (bt_build_trust_command (NULL));
  g_assert_null (bt_build_trust_command ("invalido"));
  g_assert_cmpint (bt_classify_trust_output (
    "Pairing successful", ""), ==, BT_TRUST_OK);
  g_assert_cmpint (bt_classify_trust_output (
    "Failed to pair: AlreadyExists", ""), ==, BT_TRUST_OK);
  g_assert_cmpint (bt_classify_trust_output (
    "Failed: authentication failed", ""), ==, BT_TRUST_AUTH_FAILED);
  g_assert_cmpint (bt_classify_trust_output (
    "No agent available", ""), ==, BT_TRUST_NO_AGENT);
  g_assert_cmpint (bt_classify_trust_output (
    "Failed to trust", ""), ==, BT_TRUST_FAILED);

  push_fakebin ();
  g_assert_cmpint (bt_trust_device ("11:22:33:44:55:66"), ==, BT_TRUST_OK);
  g_setenv ("QS_FAKE_BT_TRUST_FAIL", "1", TRUE);
  g_assert_cmpint (bt_trust_device ("11:22:33:44:55:66"), ==,
                   BT_TRUST_FAILED);
  g_unsetenv ("QS_FAKE_BT_TRUST_FAIL");
  g_assert_cmpint (bt_trust_device (NULL), ==, BT_TRUST_FAILED);
  pop_path ();
}

static void
test_rename_helpers (void)
{
  gchar *cmd;

  g_assert_true (bt_alias_is_valid ("FoneNovo"));
  g_assert_false (bt_alias_is_valid (NULL));
  g_assert_false (bt_alias_is_valid ("   "));
  g_assert_false (bt_alias_is_valid ("com\nquebra"));
  cmd = bt_build_rename_command ("11:22:33:44:55:66", "Fone Novo");
  g_assert_nonnull (cmd);
  g_assert_true (strstr (cmd, "set-alias") != NULL);
  g_assert_true (strstr (cmd, "--timeout 5") != NULL);
  g_free (cmd);
  g_assert_null (bt_build_rename_command (NULL, "x"));
  g_assert_null (bt_build_rename_command ("11:22:33:44:55:66", NULL));
  g_assert_cmpint (bt_classify_rename_output (
    "Changing alias succeeded", ""), ==, BT_RENAME_OK);
  g_assert_cmpint (bt_classify_rename_output (
    "Failed: authentication failed", ""), ==, BT_RENAME_AUTH_FAILED);
  g_assert_cmpint (bt_classify_rename_output (
    "No agent available", ""), ==, BT_RENAME_NO_AGENT);
  g_assert_cmpint (bt_classify_rename_output (
    "No such device", ""), ==, BT_RENAME_NO_DEVICE);
  g_assert_cmpint (bt_classify_rename_output (
    "Failed to set alias", ""), ==, BT_RENAME_FAILED);

  push_fakebin ();
  g_assert_cmpint (bt_rename_device ("11:22:33:44:55:66", "FoneNovo"),
                   ==, BT_RENAME_OK);
  g_assert_cmpint (bt_rename_device (NULL, "x"), ==, BT_RENAME_FAILED);
  g_assert_cmpint (bt_rename_device ("11:22:33:44:55:66", NULL),
                   ==, BT_RENAME_FAILED);
  pop_path ();
}

static gboolean
stub_rename_dbus_ok (const gchar *mac,
                     const gchar *alias,
                     GError     **error)
{
  (void) mac;
  (void) alias;
  (void) error;
  return TRUE;
}

static void
test_rename_fallback_named (void)
{
  BtRenameStatus st;
  GError        *error = NULL;

  push_fakebin ();
  g_setenv ("QS_FAKE_BT_ALIAS_FAIL", "1", TRUE);
  bt_rename_set_dbus_fn (stub_rename_dbus_ok);
  st = bt_rename_device ("11:22:33:44:55:66", "FoneFallback");
  g_assert_cmpint (st, ==, BT_RENAME_OK);
  bt_rename_set_dbus_fn (NULL);
  g_unsetenv ("QS_FAKE_BT_ALIAS_FAIL");
  g_assert_false (bt_rename_via_dbus (NULL, "x", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_false (bt_rename_via_dbus ("11:22:33:44:55:66", "  ", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  pop_path ();
}

static void
test_agent_path_matches (void)
{
  gchar *target;

  target = qs_bt_agent_device_path_for_mac ("11:22:33:44:55:66");
  g_assert_nonnull (target);
  g_assert_cmpstr (target, ==,
                   "/org/bluez/hci0/dev_11_22_33_44_55_66");
  g_assert_true (qs_bt_agent_path_matches (
    target, "/org/bluez/hci0/dev_11_22_33_44_55_66"));
  g_assert_true (qs_bt_agent_path_matches (
    target, "/org/bluez/hci1/dev_11_22_33_44_55_66"));
  g_assert_false (qs_bt_agent_path_matches (
    target, "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_01"));
  g_assert_false (qs_bt_agent_path_matches (target, NULL));
  g_assert_false (qs_bt_agent_path_matches (NULL, target));
  g_assert_false (qs_bt_agent_path_matches ("", ""));
  g_assert_false (qs_bt_agent_is_target (
    NULL, "/org/bluez/hci0/dev_11_22_33_44_55_66"));
  g_assert_null (qs_bt_agent_device_path_for_mac (NULL));
  g_assert_null (qs_bt_agent_device_path_for_mac ("curto"));
  g_assert_null (qs_bt_agent_device_path_for_mac ("11-22-33-44-55-66"));
  g_assert_null (qs_bt_agent_get_target (NULL));
  g_free (target);
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
  g_test_add_func ("/bt/signal_exact_true", test_signal_exact_true);
  g_test_add_func ("/bt/signal_exact_false", test_signal_exact_false);
  g_test_add_func ("/bt/signal_case_insensitive",
                   test_signal_case_insensitive);
  g_test_add_func ("/bt/signal_hci_prefix", test_signal_hci_prefix);
  g_test_add_func ("/bt/signal_other_device", test_signal_other_device);
  g_test_add_func ("/bt/signal_strange_path", test_signal_strange_path);
  g_test_add_func ("/bt/signal_no_connected", test_signal_no_connected);
  g_test_add_func ("/bt/signal_wrong_type", test_signal_wrong_type);
  g_test_add_func ("/bt/signal_nulls", test_signal_nulls);
  g_test_add_func ("/bt/dash_mac_filter", test_dash_mac_filter);
  g_test_add_func ("/bt/pair_fail", test_pair_fail);
  g_test_add_func ("/bt/trust_verb", test_trust_verb);
  g_test_add_func ("/bt/alias_verb", test_alias_verb);
  g_test_add_func ("/bt/page_dict_match", test_page_dict_match);
  g_test_add_func ("/bt/page_properties_emit", test_page_properties_emit);
  g_test_add_func ("/bt/trust_helpers", test_trust_helpers);
  g_test_add_func ("/bt/rename_helpers", test_rename_helpers);
  g_test_add_func ("/bt/rename_fallback", test_rename_fallback_named);
  g_test_add_func ("/bt/agent_path", test_agent_path_matches);
  return g_test_run ();
}
