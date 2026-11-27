/* test-pure-gaps.c — lacunas headless dos backends/DATA puros (N-001/D-006).
 *
 * Cobre ramos sem teste em qs-sysmon-data.c (filtro de FS rede/loop/fuse,
 * dedup de bind mounts, swaps só-cabeçalho, unescape vazio), em
 * qs-monitor-data.c (snapshot NULL, header --off, current via '*', linha
 * perdida, primeiro '*' vence, modos/validadores), em qs-wifi-backend.c
 * (sinal inválido, SSID com escape, duplicata fraca, saída vazia) e em
 * qs-bt-backend.c (ferramenta ausente, classificação de erro). Nenhum
 * binário real é executado: seams QS_SYSMON_* e PATH vazio. Sem display.
 */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "qs-bt-backend.h"
#include "qs-core.h"
#include "qs-monitor-data.h"
#include "qs-sysmon-data.h"
#include "qs-wifi-backend.h"

static gchar *tmp_root = NULL;
static gchar *saved_path = NULL;

static gchar *
write_fixture (const gchar *name,
               const gchar *content)
{
  gchar *path = g_build_filename (tmp_root, name, NULL);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  return path;
}

static void
push_empty_path (void)
{
  gchar *empty;

  if (saved_path == NULL)
    saved_path = g_strdup (g_getenv ("PATH"));
  empty = g_dir_make_tmp ("qs-pure-gaps-bin-XXXXXX", NULL);
  g_assert_nonnull (empty);
  g_setenv ("PATH", empty, TRUE);
  g_free (empty);
}

static void
pop_path (void)
{
  g_setenv ("PATH", saved_path, TRUE);
}

static guint
count_needle (const gchar *haystack,
              const gchar *needle)
{
  const gchar *p = haystack;
  guint        n = 0;

  g_assert_nonnull (haystack);
  while ((p = strstr (p, needle)) != NULL)
    {
      n++;
      p += strlen (needle);
    }
  return n;
}

static SysmonDisk *
find_disk (GPtrArray   *disks,
           const gchar *name)
{
  guint i;

  for (i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      if (g_strcmp0 (d->name, name) == 0)
        return d;
    }
  return NULL;
}

/* --- sysmon: filtro de filesystems --- */

static void
test_sysmon_skip_network_fs (void)
{
  gchar     *mounts;
  gchar     *missing;
  gchar     *key;
  GPtrArray *disks;

  mounts = write_fixture ("gaps-mounts-skip",
                          "/dev/sda1 /tmp ext4 rw 0 0\n"
                          "/dev/sdb1 /tmp fuseblk rw 0 0\n"
                          "/dev/nfsdisk /tmp nfs rw 0 0\n"
                          "/dev/cifsdisk /tmp cifs rw 0 0\n"
                          "/dev/smbdisk /tmp smbfs rw 0 0\n"
                          "/dev/sshdisk /tmp sshfs rw 0 0\n"
                          "/dev/fusedisk /tmp fuse.gvfsd-fuse rw 0 0\n"
                          "/dev/fuseblkdisk /tmp fuseblk rw 0 0\n"
                          "/dev/loop0 /tmp ext4 rw 0 0\n"
                          "//srv/share /tmp ext4 rw 0 0\n"
                          "nas:/export /tmp ext4 rw 0 0\n"
                          "/dev/tmpdisk /tmp tmpfs rw 0 0\n"
                          "/dev/procdisk /tmp proc rw 0 0\n");
  missing = g_build_filename (tmp_root, "gaps-swaps-ausente", NULL);
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", missing, TRUE);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  g_assert_cmpuint (disks->len, ==, 3);
  g_assert_nonnull (find_disk (disks, "sda1"));
  g_assert_nonnull (find_disk (disks, "sdb1"));
  g_assert_nonnull (find_disk (disks, "fuseblkdisk"));
  g_assert_null (find_disk (disks, "loop0"));
  sysmon_free_disks (disks);

  key = sysmon_mounts_key ();
  g_assert_nonnull (key);
  g_assert_nonnull (strstr (key, "sda1|"));
  g_assert_nonnull (strstr (key, "sdb1|"));
  g_assert_nonnull (strstr (key, "fuseblkdisk|"));
  g_assert_null (strstr (key, "loop0"));
  g_assert_null (strstr (key, "nfsdisk"));
  g_assert_null (strstr (key, "cifsdisk"));
  g_assert_null (strstr (key, "smbdisk"));
  g_assert_null (strstr (key, "sshdisk"));
  g_assert_null (strstr (key, "fusedisk"));
  g_assert_null (strstr (key, "srv"));
  g_assert_null (strstr (key, "nas:"));
  g_assert_null (strstr (key, "tmpdisk"));
  g_assert_null (strstr (key, "procdisk"));
  g_assert_cmpuint (count_needle (key, "sda1|"), ==, 1);
  g_free (key);

  sysmon_free_disks (NULL);
  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (missing);
  g_free (mounts);
}

static void
test_sysmon_dedup_bind_mount (void)
{
  gchar     *mounts;
  gchar     *missing;
  gchar     *key;
  GPtrArray *disks;

  mounts = write_fixture ("gaps-mounts-dedup",
                          "/dev/sda1 /tmp ext4 rw 0 0\n"
                          "/dev/sda1 / ext4 rw 0 0\n");
  missing = g_build_filename (tmp_root, "gaps-swaps-ausente", NULL);
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", missing, TRUE);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  g_assert_cmpuint (disks->len, ==, 1);
  g_assert_nonnull (find_disk (disks, "sda1"));
  sysmon_free_disks (disks);

  key = sysmon_mounts_key ();
  g_assert_nonnull (key);
  g_assert_cmpuint (count_needle (key, "sda1|"), ==, 1);
  g_free (key);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (missing);
  g_free (mounts);
}

static void
test_sysmon_swaps_header_only (void)
{
  gchar     *swaps;
  GPtrArray *sw;

  swaps = write_fixture ("gaps-swaps-header",
                         "Filename Type Size Used Priority\n");
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  sw = sysmon_read_swaps ();
  g_assert_nonnull (sw);
  g_assert_cmpuint (sw->len, ==, 0);
  sysmon_free_disks (sw);

  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (swaps);
}

static void
test_unescape_edge (void)
{
  gchar *empty = unescape_mount ("");
  gchar *slash = unescape_mount ("\\134");

  g_assert_cmpstr (empty, ==, "");
  g_assert_cmpstr (slash, ==, "\\");
  g_free (empty);
  g_free (slash);
}

/* --- monitor-data: ramos do snapshot --- */

static void
test_snap_null (void)
{
  QsDisplaySnap *snap = qs_display_snap_new (NULL);

  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 0);
  g_assert_cmpint (snap->n_active, ==, 0);
  qs_display_snap_free (snap);
}

static void
test_snap_off_header (void)
{
  QsDisplaySnap *snap;
  QsMonInfo     *dp;

  snap = qs_display_snap_new ("Screen 0: minimum 8 x 8, current 3840 x 1080\n"
                              "DP-1 connected (normal left inverted right) "
                              "520mm x 290mm\n"
                              "   1920x1080     60.00\n");
  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 1);
  g_assert_cmpint (snap->n_active, ==, 0);
  dp = qs_snap_find (snap, "DP-1");
  g_assert_nonnull (dp);
  g_assert_false (dp->header_active);
  g_assert_false (dp->active);
  g_assert_null (dp->current);
  g_assert_cmpuint (g_list_length (dp->modes), ==, 1);
  qs_display_snap_free (snap);
}

static void
test_snap_active_via_current (void)
{
  QsDisplaySnap *snap;
  QsMonInfo     *dp;

  snap = qs_display_snap_new ("Screen 0: minimum 8 x 8, current 1920 x 1080\n"
                              "DP-1 connected (normal left inverted right) "
                              "520mm x 290mm\n"
                              "   1920x1080     60.00*+\n"
                              "   1280x720      60.00\n");
  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 1);
  g_assert_cmpint (snap->n_active, ==, 1);
  dp = qs_snap_find (snap, "DP-1");
  g_assert_nonnull (dp);
  g_assert_false (dp->header_active);
  g_assert_true (dp->active);
  g_assert_cmpstr (dp->current, ==, "1920x1080");
  qs_display_snap_free (snap);
}

static void
test_snap_stray_and_invalid (void)
{
  QsDisplaySnap *snap;
  QsMonInfo     *dp;

  snap = qs_display_snap_new ("   1920x1080     60.00*+\n"
                              "HDMI@1 connected 1920x1080+0+0 (normal)\n"
                              "   1920x1080     60.00*+\n"
                              "DP-1 connected 1920x1080+0+0 (normal)\n"
                              "   1280x720      60.00\n");
  g_assert_nonnull (snap);
  g_assert_cmpint (snap->n_connected, ==, 1);
  dp = qs_snap_find (snap, "DP-1");
  g_assert_nonnull (dp);
  g_assert_true (dp->header_active);
  g_assert_true (dp->active);
  g_assert_null (dp->current);
  g_assert_null (qs_snap_find (snap, "HDMI@1"));
  qs_display_snap_free (snap);
}

static void
test_snap_first_star_wins (void)
{
  QsDisplaySnap *snap;
  QsMonInfo     *dp;

  snap = qs_display_snap_new ("Screen 0: minimum 8 x 8\n"
                              "DP-1 connected (normal)\n"
                              "   1920x1080     60.00*+\n"
                              "   1280x720      60.00*+\n");
  g_assert_nonnull (snap);
  dp = qs_snap_find (snap, "DP-1");
  g_assert_nonnull (dp);
  g_assert_cmpstr (dp->current, ==, "1920x1080");
  g_assert_cmpuint (g_list_length (dp->modes), ==, 2);
  g_assert_cmpstr ((const gchar *) dp->modes->data, ==, "1920x1080");
  qs_display_snap_free (snap);
}

static void
test_validators_edge (void)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    g_assert_true (valid_scale (SCALE_FACTORS[i]));
  g_assert_false (valid_mode ("1920x1080x"));
  g_assert_false (valid_mode ("1920X1080"));
  g_assert_false (valid_mode (" 1920x1080"));
  g_assert_false (valid_mode ("1920x1080 "));
  g_assert_true (header_has_geometry ("DP-1\tconnected\t1920x1080+0+0"
                                      "\t(normal)"));
  g_assert_false (header_has_geometry ("DP-1 connected (normal)"));
  g_assert_false (valid_monitor_shared ("HDMI@1"));
}

/* --- wifi-backend: ramos do parse --- */

static void
test_wifi_invalid_signal (void)
{
  GList   *nets = parse_wifi_list ("*:Net:abc:WPA2\n");
  WifiNet *n;

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 1);
  n = nets->data;
  g_assert_cmpstr (n->ssid, ==, "Net");
  g_assert_cmpint (n->signal, ==, 0);
  g_assert_true (n->secured && n->active);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
}

static void
test_wifi_escaped_ssid (void)
{
  GList   *nets = parse_wifi_list ("*:Caf\\:e:70:WPA2\n");
  WifiNet *n;

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 1);
  n = nets->data;
  g_assert_cmpstr (n->ssid, ==, "Caf:e");
  g_assert_cmpint (n->signal, ==, 70);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
}

static void
test_wifi_weaker_keeps_first (void)
{
  GList   *nets = parse_wifi_list ("*:Net:80:--\n:Net:50:WPA2\n");
  WifiNet *n;

  g_assert_nonnull (nets);
  g_assert_cmpuint (g_list_length (nets), ==, 1);
  n = nets->data;
  g_assert_cmpint (n->signal, ==, 80);
  g_assert_false (n->secured);
  g_assert_true (n->active);
  g_list_free_full (nets, (GDestroyNotify) wifi_net_free);
}

static void
test_wifi_empty_output (void)
{
  GList *nets = parse_wifi_list ("");

  /* Linha vazia é pulada e nada é acumulado: sem dados, sem lista. */
  g_assert_null (nets);
}

/* --- bt-backend: ferramenta ausente + classificação --- */

static void
test_bt_no_tool (void)
{
  GList    *devs;
  BtDevice  dev;
  gchar    *ctrl;

  push_empty_path ();
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  ctrl = bt_get_controller ();
  g_test_assert_expected_messages ();
  g_assert_null (ctrl);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  g_assert_false (bt_powered ("AA:BB:CC:DD:EE:FF"));
  g_test_assert_expected_messages ();

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  devs = list_bt_devices ();
  g_test_assert_expected_messages ();
  g_assert_null (devs);

  memset (&dev, 0, sizeof (dev));
  dev.mac = g_strdup ("AA:BB:CC:DD:EE:FF");
  dev.name = g_strdup ("NoTool");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "run_command failed*");
  bt_fill_flags (&dev);
  g_test_assert_expected_messages ();
  g_assert_false (dev.paired || dev.trusted || dev.connected);
  g_free (dev.mac);
  g_free (dev.name);
  pop_path ();
}

static void
test_bt_error_classification (void)
{
  g_assert_false (bt_output_is_error ("Failed to pair: AlreadyExists", ""));
  g_assert_true (bt_output_is_error (NULL, "Failed to connect: err"));
  g_assert_true (bt_output_is_error ("FAILED TO CONNECT", NULL));
  g_assert_false (bt_output_is_error ("some random text", NULL));
  g_assert_false (bt_str_contains_ci ("", "x"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-pure-gaps-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);

  g_test_add_func ("/pure-gaps/sysmon_skip_network_fs",
                   test_sysmon_skip_network_fs);
  g_test_add_func ("/pure-gaps/sysmon_dedup_bind_mount",
                   test_sysmon_dedup_bind_mount);
  g_test_add_func ("/pure-gaps/sysmon_swaps_header_only",
                   test_sysmon_swaps_header_only);
  g_test_add_func ("/pure-gaps/unescape_edge", test_unescape_edge);
  g_test_add_func ("/pure-gaps/snap_null", test_snap_null);
  g_test_add_func ("/pure-gaps/snap_off_header", test_snap_off_header);
  g_test_add_func ("/pure-gaps/snap_active_via_current",
                   test_snap_active_via_current);
  g_test_add_func ("/pure-gaps/snap_stray_and_invalid",
                   test_snap_stray_and_invalid);
  g_test_add_func ("/pure-gaps/snap_first_star_wins",
                   test_snap_first_star_wins);
  g_test_add_func ("/pure-gaps/validators_edge", test_validators_edge);
  g_test_add_func ("/pure-gaps/wifi_invalid_signal",
                   test_wifi_invalid_signal);
  g_test_add_func ("/pure-gaps/wifi_escaped_ssid", test_wifi_escaped_ssid);
  g_test_add_func ("/pure-gaps/wifi_weaker_keeps_first",
                   test_wifi_weaker_keeps_first);
  g_test_add_func ("/pure-gaps/wifi_empty_output", test_wifi_empty_output);
  g_test_add_func ("/pure-gaps/bt_no_tool", test_bt_no_tool);
  g_test_add_func ("/pure-gaps/bt_error_classification",
                   test_bt_error_classification);

  return g_test_run ();
}
