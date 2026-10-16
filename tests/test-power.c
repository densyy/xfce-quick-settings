/* test-power.c — GTest para qs-power (grupo 7, parse puro).
 * Usa as 3 funções expostas no touch-up (sem static). */
#include <glib.h>

#include "qs-power.h"

static const gchar *UPOWER_PATHS =
  "/org/freedesktop/UPower/devices/line_power_AC\n"
  "/org/freedesktop/UPower/devices/battery_BAT0\n";

static const gchar *DETAIL_DISCHARGING =
  "  native-path:          BAT0\n"
  "  state:               discharging\n"
  "  percentage:          55%\n"
  "  time to empty:       2.5 hours\n";

static const gchar *DETAIL_CHARGING =
  "  native-path:          BAT0\n"
  "  state:               charging\n"
  "  percentage:          20%\n"
  "  time to full:        40 minutes\n";

static void
test_battery_path (void)
{
  gchar *p = power_parse_battery_path (UPOWER_PATHS);

  g_assert_cmpstr (p, ==, "/org/freedesktop/UPower/devices/battery_BAT0");
  g_free (p);
}

static void
test_battery_path_first (void)
{
  gchar *p = power_parse_battery_path ("/org/x/battery_FOO\n/org/x/battery_BAR\n");

  g_assert_cmpstr (p, ==, "/org/x/battery_FOO");
  g_free (p);
}

static void
test_battery_path_none (void)
{
  /* "Battery" maiúsculo não contém "BAT" nem "battery" (case-sensitive). */
  g_assert_null (power_parse_battery_path ("/org/x/line_power_AC\n"));
  g_assert_null (power_parse_battery_path ("/org/x/devices/Battery_FOO\n"));
  g_assert_null (power_parse_battery_path (NULL));
}

static void
test_detail_discharging (void)
{
  gint     percent = -1;
  gchar   *state = NULL;
  gchar   *time = NULL;
  gboolean ok = power_parse_detail (DETAIL_DISCHARGING, &percent, &state, &time);

  g_assert_true (ok);
  g_assert_cmpint (percent, ==, 55);
  g_assert_cmpstr (state, ==, "discharging");
  g_assert_cmpstr (time, ==, "2.5 hours");
  g_free (state);
  g_free (time);
}

static void
test_detail_charging (void)
{
  gint     percent = -1;
  gchar   *state = NULL;
  gchar   *time = NULL;
  gboolean ok = power_parse_detail (DETAIL_CHARGING, &percent, &state, &time);

  g_assert_true (ok);
  g_assert_cmpint (percent, ==, 20);
  g_assert_cmpstr (state, ==, "charging");
  /* Charging usa "time to full". */
  g_assert_cmpstr (time, ==, "40 minutes");
  g_free (state);
  g_free (time);
}

static void
test_detail_bad (void)
{
  gint     percent = -1;
  gchar   *state = NULL;
  gchar   *time = NULL;

  g_assert_false (power_parse_detail ("  state: discharging\n", &percent, &state, &time));
  g_assert_false (power_parse_detail (NULL, &percent, &state, &time));
  g_free (state);
  g_free (time);
}

static void
test_icon_charging (void)
{
  g_assert_cmpstr (battery_icon_for_level (100, TRUE), ==, "battery-full-charged-symbolic");
  g_assert_cmpstr (battery_icon_for_level (5, TRUE), ==, "battery-empty-charging-symbolic");
  g_assert_cmpstr (battery_icon_for_level (20, TRUE), ==, "battery-caution-charging-symbolic");
  g_assert_cmpstr (battery_icon_for_level (40, TRUE), ==, "battery-low-charging-symbolic");
  g_assert_cmpstr (battery_icon_for_level (80, TRUE), ==, "battery-good-charging-symbolic");
  g_assert_cmpstr (battery_icon_for_level (81, TRUE), ==, "battery-full-charging-symbolic");
}

static void
test_icon_discharging (void)
{
  g_assert_cmpstr (battery_icon_for_level (100, FALSE), ==, "battery-full-charged-symbolic");
  g_assert_cmpstr (battery_icon_for_level (81, FALSE), ==, "battery-full-symbolic");
  g_assert_cmpstr (battery_icon_for_level (80, FALSE), ==, "battery-good-symbolic");
  g_assert_cmpstr (battery_icon_for_level (40, FALSE), ==, "battery-low-symbolic");
  g_assert_cmpstr (battery_icon_for_level (20, FALSE), ==, "battery-caution-symbolic");
  g_assert_cmpstr (battery_icon_for_level (5, FALSE), ==, "battery-empty-symbolic");
  g_assert_cmpstr (battery_icon_for_level (0, FALSE), ==, "battery-empty-symbolic");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/power/battery_path", test_battery_path);
  g_test_add_func ("/power/battery_path_first", test_battery_path_first);
  g_test_add_func ("/power/battery_path_none", test_battery_path_none);
  g_test_add_func ("/power/detail_discharging", test_detail_discharging);
  g_test_add_func ("/power/detail_charging", test_detail_charging);
  g_test_add_func ("/power/detail_bad", test_detail_bad);
  g_test_add_func ("/power/icon_charging", test_icon_charging);
  g_test_add_func ("/power/icon_discharging", test_icon_discharging);
  return g_test_run ();
}
