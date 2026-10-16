/* test-brightness.c — GTest para bright_extract_output (grupo 9).
 * Função exposta via src/qs-brightness.h (touch-up, sem mudar lógica). */
#include <glib.h>

#include "qs-brightness.h"

static const gchar *DUMP_TWO =
  "Screen 0: minimum 8 x 8, current 3840 x 1080, maximum 32767 x 32767\n"
  "HDMI-1 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis)\n"
  "   1920x1080     60.00*+\n"
  "DP-1 connected 1920x1080+1920+0 (normal left inverted right x axis y axis)\n";

static void
test_first_connected (void)
{
  gchar *name = bright_extract_output (DUMP_TWO);

  g_assert_cmpstr (name, ==, "HDMI-1");
  g_free (name);
}

static void
test_none_connected (void)
{
  /* " disconnected" não casa com " connected" (exige o espaço antes). */
  gchar *name = bright_extract_output ("VGA-1 disconnected (normal left inverted right)\n");

  g_assert_null (name);
}

static void
test_null (void)
{
  g_assert_null (bright_extract_output (NULL));
}

static void
test_invalid_name_skipped (void)
{
  /* "HDMI@1" falha em valid_monitor_shared: pula para o próximo. */
  gchar *name = bright_extract_output ("HDMI@1 connected 1920x1080+0+0\n"
                                       "DP-1 connected 1920x1080+0+0\n");

  g_assert_cmpstr (name, ==, "DP-1");
  g_free (name);
}

static void
test_empty (void)
{
  g_assert_null (bright_extract_output (""));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/brightness/first_connected", test_first_connected);
  g_test_add_func ("/brightness/none_connected", test_none_connected);
  g_test_add_func ("/brightness/null", test_null);
  g_test_add_func ("/brightness/invalid_name_skipped", test_invalid_name_skipped);
  g_test_add_func ("/brightness/empty", test_empty);
  return g_test_run ();
}
