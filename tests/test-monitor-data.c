/* test-monitor-data.c — GTest para qs-monitor-data (grupo 6).
 * Puro: validadores + snapshot xrandr a partir de string. */
#include <string.h>

#include <glib.h>

#include "qs-monitor-data.h"

static const gchar *DUMP_TWO =
  "Screen 0: minimum 8 x 8, current 3840 x 1080, maximum 32767 x 32767\n"
  "HDMI-1 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis) 520mm x 290mm\n"
  "   1920x1080     60.00*+  50.00\n"
  "   1280x720      60.00\n"
  "DP-1 connected 1920x1080+1920+0 (normal left inverted right x axis y axis) 520mm x 290mm\n"
  "   1920x1080     60.00*+\n"
  "VGA-1 disconnected (normal left inverted right x axis y axis)\n";

static void
test_shared_valid (void)
{
  g_assert_true (valid_monitor_shared ("HDMI-1"));
  g_assert_true (valid_monitor_shared ("eDP_1.2-3"));
}

static void
test_shared_invalid (void)
{
  gchar buf[70];

  g_assert_false (valid_monitor_shared (""));
  g_assert_false (valid_monitor_shared (NULL));
  g_assert_false (valid_monitor_shared ("a b"));
  g_assert_false (valid_monitor_shared ("HDMI@1"));
  memset (buf, 'a', 65);
  buf[65] = '\0';
  g_assert_false (valid_monitor_shared (buf));
  memset (buf, 'a', 64);
  buf[64] = '\0';
  g_assert_true (valid_monitor_shared (buf));
}

static void
test_mode (void)
{
  g_assert_true (valid_mode ("1920x1080"));
  g_assert_false (valid_mode ("x1080"));
  g_assert_false (valid_mode ("1920x"));
  g_assert_false (valid_mode (""));
  g_assert_false (valid_mode ("abc"));
  g_assert_false (valid_mode (NULL));
}

static void
test_scale (void)
{
  g_assert_true (valid_scale ("1"));
  g_assert_true (valid_scale ("1.25"));
  g_assert_true (valid_scale ("1.5"));
  g_assert_true (valid_scale ("1.75"));
  g_assert_true (valid_scale ("2"));
  g_assert_false (valid_scale ("1.0"));
  g_assert_false (valid_scale ("3"));
  g_assert_false (valid_scale (""));
  g_assert_false (valid_scale (NULL));
  for (gsize i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    g_assert_true (valid_scale (SCALE_FACTORS[i]));
}

static void
test_geometry (void)
{
  g_assert_true (header_has_geometry ("HDMI-1 connected primary 1920x1080+0+0 (normal)"));
  g_assert_false (header_has_geometry ("HDMI-1 connected (normal left inverted right)"));
  g_assert_false (header_has_geometry (NULL));
}

static void
test_snap_two (void)
{
  QsDisplaySnap *snap = qs_display_snap_new (DUMP_TWO);
  QsMonInfo     *hdmi;
  QsMonInfo     *dp;

  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 2);
  g_assert_cmpint (snap->n_active, ==, 2);

  hdmi = qs_snap_find (snap, "HDMI-1");
  g_assert_nonnull (hdmi);
  g_assert_true (hdmi->active);
  g_assert_true (hdmi->header_active);
  g_assert_cmpstr (hdmi->current, ==, "1920x1080");
  g_assert_cmpuint (g_list_length (hdmi->modes), ==, 2);

  dp = qs_snap_find (snap, "DP-1");
  g_assert_nonnull (dp);
  g_assert_true (dp->active);
  g_assert_cmpstr (dp->current, ==, "1920x1080");

  /* Desconectado não entra no snapshot. */
  g_assert_null (qs_snap_find (snap, "VGA-1"));
  g_assert_null (qs_snap_find (snap, "NOPE"));
  g_assert_null (qs_snap_find (snap, NULL));
  g_assert_null (qs_snap_find (NULL, "HDMI-1"));
  qs_display_snap_free (snap);
}

static void
test_snap_empty (void)
{
  QsDisplaySnap *snap = qs_display_snap_new ("");

  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 0);
  qs_display_snap_free (snap);
}

static void
test_free_null (void)
{
  /* Frees são NULL-safe. */
  qs_display_snap_free (NULL);
  qs_mon_info_free (NULL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/monitor-data/shared_valid", test_shared_valid);
  g_test_add_func ("/monitor-data/shared_invalid", test_shared_invalid);
  g_test_add_func ("/monitor-data/mode", test_mode);
  g_test_add_func ("/monitor-data/scale", test_scale);
  g_test_add_func ("/monitor-data/geometry", test_geometry);
  g_test_add_func ("/monitor-data/snap_two", test_snap_two);
  g_test_add_func ("/monitor-data/snap_empty", test_snap_empty);
  g_test_add_func ("/monitor-data/free_null", test_free_null);
  return g_test_run ();
}
