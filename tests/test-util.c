/* Testes do grupo 1: qs-util (parsing e comandos sync, sem main loop). */
#include <glib.h>

#include "qs-util.h"

static void
test_parse_int_valid (void)
{
  gint out = -1;

  g_assert_true (qs_parse_int_checked ("0", &out) && out == 0);
  g_assert_true (qs_parse_int_checked ("42", &out) && out == 42);
  g_assert_true (qs_parse_int_checked ("  7  ", &out) && out == 7);
  g_assert_true (qs_parse_int_checked ("+5", &out) && out == 5);
  g_assert_true (qs_parse_int_checked ("2147483647", &out) && out == G_MAXINT);
}

static void
test_parse_int_invalid (void)
{
  gint out = 1234;

  g_assert_false (qs_parse_int_checked (NULL, &out));
  g_assert_false (qs_parse_int_checked ("", &out));
  g_assert_false (qs_parse_int_checked ("   ", &out));
  g_assert_false (qs_parse_int_checked ("-", &out));
  g_assert_false (qs_parse_int_checked ("+", &out));
  g_assert_false (qs_parse_int_checked ("12x", &out));
  g_assert_false (qs_parse_int_checked ("x12", &out));
  g_assert_false (qs_parse_int_checked ("-3", &out));
  g_assert_false (qs_parse_int_checked ("4.5", &out));
  g_assert_false (qs_parse_int_checked ("2147483648", &out));
  g_assert_false (qs_parse_int_checked ("99999999999999999999", &out));
  g_assert_false (qs_parse_int_checked ("42", NULL));
  /* out intocado nas falhas */
  g_assert_cmpint (out, ==, 1234);
}

static void
test_split_basic (void)
{
  gchar **parts = split_terse_line ("IN-USE:SSID:SIGNAL:SECURITY", 4);

  g_assert_nonnull (parts);
  g_assert_cmpuint (g_strv_length (parts), ==, 4);
  g_assert_cmpstr (parts[0], ==, "IN-USE");
  g_assert_cmpstr (parts[1], ==, "SSID");
  g_assert_cmpstr (parts[2], ==, "SIGNAL");
  g_assert_cmpstr (parts[3], ==, "SECURITY");
  g_assert_null (parts[4]);
  g_strfreev (parts);
}

static void
test_split_escape (void)
{
  gchar **parts = split_terse_line ("a\\:b:c", 2);

  g_assert_nonnull (parts);
  g_assert_cmpuint (g_strv_length (parts), ==, 2);
  g_assert_cmpstr (parts[0], ==, "a:b");
  g_assert_cmpstr (parts[1], ==, "c");
  g_strfreev (parts);
}

static void
test_split_pad (void)
{
  gchar **parts = split_terse_line ("only", 3);

  g_assert_nonnull (parts);
  g_assert_cmpuint (g_strv_length (parts), ==, 3);
  g_assert_cmpstr (parts[0], ==, "only");
  g_assert_cmpstr (parts[1], ==, "");
  g_assert_cmpstr (parts[2], ==, "");
  g_strfreev (parts);
}

static void
test_split_limit (void)
{
  /* com expected=2, só o primeiro ':' separa; o resto fica literal */
  gchar **parts = split_terse_line ("a:b:c:d", 2);

  g_assert_nonnull (parts);
  g_assert_cmpuint (g_strv_length (parts), ==, 2);
  g_assert_cmpstr (parts[0], ==, "a");
  g_assert_cmpstr (parts[1], ==, "b:c:d");
  g_strfreev (parts);
}

static void
test_percent (void)
{
  g_assert_cmpint (parse_first_percent ("Volume: 55%"), ==, 55);
  g_assert_cmpint (parse_first_percent ("100%"), ==, 100);
  g_assert_cmpint (parse_first_percent ("10% 20%"), ==, 10);
  g_assert_cmpint (parse_first_percent ("abc"), ==, -1);
  g_assert_cmpint (parse_first_percent ("%"), ==, -1);
  g_assert_cmpint (parse_first_percent ("vol %"), ==, -1);
  g_assert_cmpint (parse_first_percent (NULL), ==, -1);
}

static void
test_command_exists (void)
{
  g_assert_true (command_exists ("sh"));
  g_assert_false (command_exists ("definitely-not-a-real-binary-xyz"));
}

static void
test_run_command (void)
{
  gchar *out = run_command ("printf hello");

  g_assert_cmpstr (out, ==, "hello");
  g_free (out);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  g_assert_null (run_command ("definitely-not-a-real-binary-xyz"));
  g_test_assert_expected_messages ();
}

gint
main (gint argc, gchar **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/util/parse_int/valid", test_parse_int_valid);
  g_test_add_func ("/util/parse_int/invalid", test_parse_int_invalid);
  g_test_add_func ("/util/split/basic", test_split_basic);
  g_test_add_func ("/util/split/escape", test_split_escape);
  g_test_add_func ("/util/split/pad", test_split_pad);
  g_test_add_func ("/util/split/limit", test_split_limit);
  g_test_add_func ("/util/percent", test_percent);
  g_test_add_func ("/util/command_exists", test_command_exists);
  g_test_add_func ("/util/run_command", test_run_command);
  return g_test_run ();
}
