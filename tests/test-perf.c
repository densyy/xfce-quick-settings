/* Micro-benchmarks com teto (gates de performance).
 *
 * Cada caso mede o tempo de uma operação quente e falha se exceder o
 * teto (generoso p/ máquina carregada; detecta regressões de ordem de
 * grandeza, não ruído). Tempos impressos via g_test_message.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "qs-wifi-backend.h"
#include "qs-sysmon-data.h"
#include "qs-util.h"
#include "qs-monitor-data.h"
#include "qs-notify-store.h"
#include "qs-core.h"

static void
assert_ms_under (GTimer *t, gdouble budget_ms, const gchar *what)
{
  gdouble ms = g_timer_elapsed (t, NULL) * 1000.0;

  g_test_message ("%s: %.2f ms (teto %.0f ms)", what, ms, budget_ms);
  g_assert_cmpfloat (ms, <, budget_ms);
}

static void
test_parse_wifi_1k (void)
{
  GString *in = g_string_new (NULL);
  gchar *joined;
  GList *nets;
  GTimer *t;

  for (gint i = 0; i < 1000; i++)
    g_string_append_printf (in, " :NET%04d:70:WPA2\n", i);
  joined = g_string_free (in, FALSE);
  t = g_timer_new ();
  nets = parse_wifi_list (joined);
  assert_ms_under (t, 100.0, "parse_wifi_list 1k");
  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 1000);
  g_list_free_full (nets, wifi_net_free);
  g_timer_destroy (t);
  g_free (joined);
}

static void
test_unescape_long (void)
{
  GString *in = g_string_new (NULL);
  gchar *joined;
  gchar *un;
  GTimer *t;

  for (gint i = 0; i < 50000; i++)
    g_string_append (in, "\\040ABCD");
  joined = g_string_free (in, FALSE);
  t = g_timer_new ();
  un = unescape_mount (joined);
  assert_ms_under (t, 500.0, "unescape_mount 250k");
  g_assert_cmpuint (strlen (un), ==, (gsize) 250000);
  g_assert_true (g_str_has_prefix (un, " ABCD"));
  g_free (un);
  g_timer_destroy (t);
  g_free (joined);
}

static void
test_snap_big (void)
{
  GString *in = g_string_new ("Screen 0: minimum 8 x 8, current 1920 x 1080, maximum 32767 x 32767\n");
  gchar *joined;
  QsDisplaySnap *snap;
  GTimer *t;
  const QsMonInfo *m;

  for (gint i = 0; i < 40; i++)
    {
      g_string_append_printf (in, "HDMI-%d connected primary 1920x1080+0+0 (normal left inverted right x axis y axis) 500mm x 300mm\n", i);
      for (gint j = 0; j < 8; j++)
        g_string_append_printf (in, "   1920x1080     60.00%s  59.94\n", (i == 0 && j == 0) ? "*" : "");
    }
  joined = g_string_free (in, FALSE);
  t = g_timer_new ();
  snap = qs_display_snap_new (joined);
  assert_ms_under (t, 200.0, "qs_display_snap_new 40x8");
  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 40);
  m = qs_snap_find (snap, "HDMI-5");
  g_assert_nonnull (m);
  qs_display_snap_free (snap);
  g_timer_destroy (t);
  g_free (joined);
}

static void
test_flat_long (void)
{
  GString *in = g_string_new (NULL);
  gchar *joined;
  gchar *flat;
  GTimer *t;

  for (gint i = 0; i < 10000; i++)
    g_string_append (in, "aaaaaaaaaa\r\n");
  joined = g_string_free (in, FALSE);
  t = g_timer_new ();
  flat = flat_text ((const guchar *) joined);
  assert_ms_under (t, 200.0, "flat_text 120k");
  g_assert_cmpuint (strlen (flat), ==, (gsize) 120000);
  g_free (flat);
  g_timer_destroy (t);
  g_free (joined);
}

static void
test_split_many (void)
{
  GString *in = g_string_new (NULL);
  gchar *joined;
  gchar **parts;
  GTimer *t;
  gint count = 0;

  for (gint i = 0; i < 49999; i++)
    g_string_append (in, "a:");
  g_string_append_c (in, 'a');
  joined = g_string_free (in, FALSE);
  t = g_timer_new ();
  parts = split_terse_line (joined, 50000);
  assert_ms_under (t, 1000.0, "split_terse_line 50k");
  while (parts[count] != NULL)
    count++;
  g_assert_cmpint (count, ==, 50000);
  g_strfreev (parts);
  g_timer_destroy (t);
  g_free (joined);
}

static void
test_read_memory (void)
{
  guint64 total = 0;
  guint64 avail = 0;
  gboolean ok;
  GTimer *t = g_timer_new ();

  ok = sysmon_read_memory (&total, &avail);
  assert_ms_under (t, 50.0, "sysmon_read_memory");
  g_assert_true (ok);
  g_assert_cmpuint (total, >, 0);
  g_timer_destroy (t);
}

static void
test_read_cpu_twice (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  gdouble v1;
  gdouble v2;
  GTimer *t = g_timer_new ();

  v1 = sysmon_read_cpu (qs);
  v2 = sysmon_read_cpu (qs);
  assert_ms_under (t, 100.0, "sysmon_read_cpu x2");
  g_assert_cmpfloat (v1, >=, 0.0);
  g_assert_cmpfloat (v1, <=, 100.0);
  g_assert_cmpfloat (v2, >=, 0.0);
  g_assert_cmpfloat (v2, <=, 100.0);
  g_timer_destroy (t);
  g_free (qs);
}

static void
test_read_disks (void)
{
  GPtrArray *disks;
  GTimer *t = g_timer_new ();

  disks = sysmon_read_disks ();
  assert_ms_under (t, 2000.0, "sysmon_read_disks");
  g_assert_nonnull (disks);
  for (guint i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      g_assert_nonnull (d->name);
      g_assert_nonnull (d->text);
      g_assert_cmpfloat (d->fraction, >=, 0.0);
      g_assert_cmpfloat (d->fraction, <=, 1.0);
    }
  sysmon_free_disks (disks);
  g_timer_destroy (t);
}

static void
test_mounts_key (void)
{
  gchar *key;
  GTimer *t = g_timer_new ();

  key = sysmon_mounts_key ();
  assert_ms_under (t, 50.0, "sysmon_mounts_key");
  g_assert_nonnull (key);
  g_free (key);
  g_timer_destroy (t);
}

static void
test_scale_valid (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    g_assert_true (valid_scale (SCALE_FACTORS[i]));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/perf/parse_wifi_1k", test_parse_wifi_1k);
  g_test_add_func ("/perf/unescape_long", test_unescape_long);
  g_test_add_func ("/perf/snap_big", test_snap_big);
  g_test_add_func ("/perf/flat_long", test_flat_long);
  g_test_add_func ("/perf/split_many", test_split_many);
  g_test_add_func ("/perf/read_memory", test_read_memory);
  g_test_add_func ("/perf/read_cpu_twice", test_read_cpu_twice);
  g_test_add_func ("/perf/read_disks", test_read_disks);
  g_test_add_func ("/perf/mounts_key", test_mounts_key);
  g_test_add_func ("/perf/scale_valid", test_scale_valid);
  return g_test_run ();
}
