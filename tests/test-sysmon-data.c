/* test-sysmon-data.c — GTest para qs-sysmon-data (grupo 5).
 * Puros + leituras live de /proc (sem widgets, sem display). */
#include <string.h>

#include <glib.h>

#include "qs-sysmon-data.h"
#include "qs-core.h"

static void
test_unescape_basic (void)
{
  gchar *r = unescape_mount ("a\\040b\\011c\\012d\\134e");

  g_assert_cmpstr (r, ==, "a b\tc\nd\\e");
  g_free (r);
}

static void
test_unescape_other_octal (void)
{
  /* \077 não é mapeado: passa os 4 chars literalmente. */
  gchar *r = unescape_mount ("a\\077b");

  g_assert_cmpstr (r, ==, "a\\077b");
  g_free (r);
}

static void
test_unescape_plain (void)
{
  gchar *r = unescape_mount ("/dev/sda1");

  g_assert_cmpstr (r, ==, "/dev/sda1");
  g_free (r);
}

static void
test_unescape_lone_backslash (void)
{
  /* '\b' não é sequência \0XX: passa literal. */
  gchar *r = unescape_mount ("a\\b");

  g_assert_cmpstr (r, ==, "a\\b");
  g_free (r);
}

static void
test_bytes_text (void)
{
  gchar *t = sysmon_bytes_text (1073741824ULL, 2147483648ULL);

  g_assert_cmpstr (t, ==, "1.00 / 2.00 GB");
  g_free (t);
}

static void
test_read_memory (void)
{
  guint64   total = 0;
  guint64   avail = 0;
  gboolean  ok = sysmon_read_memory (&total, &avail);

  g_assert_true (ok);
  g_assert_cmpuint (total, >, 0);
  g_assert_cmpuint (avail, >, 0);
  g_assert_true (avail <= total);
}

static void
test_read_cpu (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);
  gdouble        c1 = sysmon_read_cpu (qs);
  gdouble        c2 = sysmon_read_cpu (qs);

  g_assert_cmpfloat (c1, >=, 0.0);
  g_assert_cmpfloat (c1, <=, 100.0);
  g_assert_cmpfloat (c2, >=, 0.0);
  g_assert_cmpfloat (c2, <=, 100.0);
  g_free (qs);
}

static void
test_read_disks (void)
{
  GPtrArray *disks = sysmon_read_disks ();
  guint      i;

  g_assert_nonnull (disks);
  for (i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      g_assert_nonnull (d->name);
      g_assert_nonnull (d->text);
      g_assert_cmpfloat (d->fraction, >=, 0.0);
      g_assert_cmpfloat (d->fraction, <=, 1.0);
    }
  sysmon_free_disks (disks);
}

static void
test_mounts_key (void)
{
  gchar *k = sysmon_mounts_key ();

  g_assert_nonnull (k);
  g_assert_nonnull (strchr (k, '|'));
  g_free (k);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sysmon-data/unescape_basic", test_unescape_basic);
  g_test_add_func ("/sysmon-data/unescape_other_octal", test_unescape_other_octal);
  g_test_add_func ("/sysmon-data/unescape_plain", test_unescape_plain);
  g_test_add_func ("/sysmon-data/unescape_lone_backslash", test_unescape_lone_backslash);
  g_test_add_func ("/sysmon-data/bytes_text", test_bytes_text);
  g_test_add_func ("/sysmon-data/read_memory", test_read_memory);
  g_test_add_func ("/sysmon-data/read_cpu", test_read_cpu);
  g_test_add_func ("/sysmon-data/read_disks", test_read_disks);
  g_test_add_func ("/sysmon-data/mounts_key", test_mounts_key);
  return g_test_run ();
}
