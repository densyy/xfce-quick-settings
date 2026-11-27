/* test-sysmon-nvme.c — regressão NVMe + swap com fixtures via env.
 *
 * Usa o seam QS_SYSMON_MOUNTS / QS_SYSMON_SWAPS de qs-sysmon-data.c:
 * nenhum binário real é executado, só arquivos temporários.
 * Fixture base: NVMe com EFI pequena e raiz ext4, mais swap em partição.
 * Sem display: testes puros, sem widgets.
 */
#include <string.h>
#include <sys/statvfs.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "qs-sysmon-data.h"
#include "qs-core.h"

static gchar *tmp_root = NULL;

static gchar *
write_fixture (const gchar *name,
               const gchar *content)
{
  gchar *path = g_build_filename (tmp_root, name, NULL);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  return path;
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

static void
check_disk_sane (SysmonDisk *d)
{
  g_assert_nonnull (d->name);
  g_assert_nonnull (d->text);
  g_assert_cmpfloat (d->fraction, >=, 0.0);
  g_assert_cmpfloat (d->fraction, <=, 1.0);
}

/* (a) swap agrega /proc/swaps em uma linha "swap".
 * Antes do fix a linha não existia. */
static void
test_swap_line (void)
{
  gchar *mounts;
  gchar *swaps;
  GPtrArray *sw;
  GPtrArray *disks;
  SysmonDisk *one;
  SysmonDisk *in_disks;

  mounts = write_fixture ("nvme-mounts-a",
                          "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-a",
                         "Filename Type Size Used Priority\n"
                         "/dev/nvme0n1p3 partition 8388604 0 -2\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  sw = sysmon_read_swaps ();
  g_assert_nonnull (sw);
  g_assert_cmpuint (sw->len, ==, 1);
  one = g_ptr_array_index (sw, 0);
  g_assert_cmpstr (one->name, ==, "swap");
  g_assert_cmpfloat (one->fraction, ==, 0.0);
  g_assert_nonnull (strstr (one->text, "GB"));
  check_disk_sane (one);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  in_disks = find_disk (disks, "swap");
  g_assert_nonnull (in_disks);
  g_assert_cmpstr (in_disks->text, ==, one->text);
  g_assert_nonnull (find_disk (disks, "nvme0n1p2"));
  sysmon_free_disks (disks);
  sysmon_free_disks (sw);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

/* (b) EFI pequena formata em MB, nunca "0.00 GB".
 * sysmon_size_text usa MB abaixo de 1 GiB. */
static void
test_efi_small_formats_mb (void)
{
  gchar *efi;
  gchar *root;
  gchar *edge_mb;
  gchar *edge_gb;

  efi = sysmon_bytes_text (536870912ULL, 536870912ULL);
  g_assert_cmpstr (efi, ==, "512 MB / 512 MB");
  g_assert_null (strstr (efi, "GB"));
  g_assert_null (strstr (efi, "0.00 GB"));
  g_free (efi);

  root = sysmon_bytes_text (104857600ULL, 536870912ULL);
  g_assert_cmpstr (root, ==, "100 MB / 512 MB");
  g_assert_null (strstr (root, "GB"));
  g_assert_null (strstr (root, "0.00"));
  g_free (root);

  edge_mb = sysmon_bytes_text (1073741823ULL, 1073741823ULL);
  g_assert_nonnull (strstr (edge_mb, "MB"));
  g_assert_null (strstr (edge_mb, "GB"));
  g_free (edge_mb);

  edge_gb = sysmon_bytes_text (1073741824ULL, 1073741824ULL);
  g_assert_cmpstr (edge_gb, ==, "1.00 GB / 1.00 GB");
  g_free (edge_gb);
}

/* (c) mounts_key muda quando o swap muda (assinatura de swaps na chave).
 * Sem isso a linha swap congelaria na UI. */
static void
test_mounts_key_changes_with_swap (void)
{
  gchar *mounts;
  gchar *swaps;
  gchar *key_before;
  gchar *key_after;

  mounts = write_fixture ("nvme-mounts-c",
                          "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-c",
                         "Filename Type Size Used Priority\n"
                         "/dev/nvme0n1p3 partition 8388604 0 -2\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  key_before = sysmon_mounts_key ();
  g_assert_nonnull (key_before);
  g_assert_nonnull (strstr (key_before, "nvme0n1p2|"));
  g_assert_nonnull (strstr (key_before, "swap:"));

  g_assert_true (g_file_set_contents (swaps,
                                      "Filename Type Size Used Priority\n"
                                      "/dev/nvme0n1p3 partition 8388604 1048576 -2\n",
                                      -1, NULL));
  key_after = sysmon_mounts_key ();
  g_assert_nonnull (key_after);
  g_assert_cmpstr (key_before, !=, key_after);
  g_assert_nonnull (strstr (key_after, "swap:"));

  g_free (key_before);
  g_free (key_after);
  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

/* (d) statvfs falho some da lista (skip, nunca zero).
 * Alvo inexistente não pode virar linha "0 MB" nem "0.00 GB". */
static void
test_statvfs_fail_skipped (void)
{
  gchar *mounts;
  gchar *swaps;
  GPtrArray *disks;
  SysmonDisk *bogus;
  SysmonDisk *root;
  guint i;

  mounts = write_fixture ("nvme-mounts-d",
                          "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n"
                          "/dev/nvme0n1p9 /nonexistent-qs-nvme-xyz ext4 rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-d",
                         "Filename Type Size Used Priority\n"
                         "/dev/nvme0n1p3 partition 8388604 0 -2\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  bogus = find_disk (disks, "nvme0n1p9");
  g_assert_null (bogus);
  root = find_disk (disks, "nvme0n1p2");
  g_assert_nonnull (root);
  for (i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      check_disk_sane (d);
      g_assert_null (strstr (d->text, "0.00 / 0.00 GB"));
      g_assert_cmpstr (d->name, !=, "nvme0n1p9");
    }
  sysmon_free_disks (disks);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

/* (e) efivarfs filtrado: nunca vira linha "0 MB / 0 MB" (D-012).
 * skip_fs[] cobre efivarfs/bpf/autofs/binfmt_misc; a chave também exclui. */
static void
test_efivarfs_filtered (void)
{
  gchar *mounts;
  gchar *swaps;
  GPtrArray *disks;
  gchar *key;
  guint i;

  mounts = write_fixture ("nvme-mounts-efivarfs",
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n"
                          "/dev/nvme0n1p1 /boot vfat rw 0 0\n"
                          "efivarfs /sys/firmware/efi/efivars efivarfs rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-efivarfs",
                         "Filename Type Size Used Priority\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  g_assert_null (find_disk (disks, "efivarfs"));
  for (i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      check_disk_sane (d);
      g_assert_null (strstr (d->text, "0 MB / 0 MB"));
    }
  sysmon_free_disks (disks);

  key = sysmon_mounts_key ();
  g_assert_nonnull (key);
  g_assert_null (strstr (key, "efivarfs|"));
  g_free (key);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

/* (g) partição pequena some, partição grande continua (D-012 2ª rodada).
 * Regra N-FIX em disks_fold_line: total < 1 GiB descarta a linha.
 * - "efivarfs" (184 KB reais em /sys/firmware/efi/efivars): ausente de
 *   sysmon_read_disks E da key (skip_fs[]).
 * - "/dev/nvme0n1p0" vfat apontando para o mesmo alvo minúsculo: fstype
 *   NÃO pulado, statvfs OK, total 184 KB < 1 GiB → ausente SÓ pela regra
 *   de tamanho. Sem ela viraria "0 MB / 0 MB". PROVA do N-FIX.
 *   (A key é parse-only, sem statvfs: "nvme0n1p0|" continua na key por
 *   desenho — filtrar tamanho na key exigiria mudar src/.)
 * - "/dev/nvme0n1p1" /boot vfat real ~4 GiB (>= 1 GiB): continua presente
 *   (guarda statvfs: máquina sem /boot grande pula só este assert). */
static void
test_small_hidden (void)
{
  gchar *mounts;
  gchar *swaps;
  GPtrArray *disks;
  gchar *key;
  SysmonDisk *boot;
  SysmonDisk *root;
  struct statvfs st;
  gboolean boot_big = FALSE;
  guint i;

  mounts = write_fixture ("nvme-mounts-small",
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n"
                          "/dev/nvme0n1p1 /boot vfat rw 0 0\n"
                          "efivarfs /sys/firmware/efi/efivars efivarfs rw 0 0\n"
                          "/dev/nvme0n1p0 /sys/firmware/efi/efivars vfat rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-small",
                         "Filename Type Size Used Priority\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  if (statvfs ("/boot", &st) == 0
      && st.f_blocks != 0 && st.f_frsize != 0
      && (guint64) st.f_blocks * st.f_frsize >= 1073741824ULL)
    boot_big = TRUE;

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  g_assert_null (find_disk (disks, "efivarfs"));
  g_assert_null (find_disk (disks, "nvme0n1p0"));
  root = find_disk (disks, "nvme0n1p2");
  g_assert_nonnull (root);
  check_disk_sane (root);
  if (boot_big)
    {
      boot = find_disk (disks, "nvme0n1p1");
      g_assert_nonnull (boot);
      check_disk_sane (boot);
      g_assert_nonnull (strstr (boot->text, "GB"));
    }
  for (i = 0; i < disks->len; i++)
    {
      SysmonDisk *d = g_ptr_array_index (disks, i);

      check_disk_sane (d);
      g_assert_null (strstr (d->text, "0 MB / 0 MB"));
    }
  sysmon_free_disks (disks);

  key = sysmon_mounts_key ();
  g_assert_nonnull (key);
  g_assert_null (strstr (key, "efivarfs|"));
  g_assert_nonnull (strstr (key, "nvme0n1p2|"));
  g_assert_nonnull (strstr (key, "nvme0n1p0|"));
  if (boot_big)
    g_assert_nonnull (strstr (key, "nvme0n1p1|"));
  g_free (key);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

/* Robustez do parse de swaps: arquivo ausente, linha malformada,
 * entrada com tamanho zero e Used maior que Size (clamp).
 * Cobre os ramos defensivos de sysmon_read_swaps. */
static void
test_swaps_robust (void)
{
  gchar *swaps;
  gchar *missing;
  GPtrArray *sw;
  GPtrArray *empty;
  GPtrArray *no_mounts;
  SysmonDisk *one;
  gchar *key;

  swaps = write_fixture ("nvme-swaps-e",
                         "Filename Type Size Used Priority\n"
                         "bogus\n"
                         "/dev/zram0 partition 0 0 100\n"
                         "/dev/nvme0n1p3 partition 8388604 99999999 -2\n");
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);
  g_unsetenv ("QS_SYSMON_MOUNTS");

  sw = sysmon_read_swaps ();
  g_assert_nonnull (sw);
  g_assert_cmpuint (sw->len, ==, 1);
  one = g_ptr_array_index (sw, 0);
  g_assert_cmpstr (one->name, ==, "swap");
  g_assert_cmpfloat (one->fraction, ==, 1.0);
  g_assert_cmpstr (one->text, ==, "8.00 GB / 8.00 GB");
  sysmon_free_disks (sw);

  missing = g_build_filename (tmp_root, "nvme-swaps-ausente", NULL);
  g_setenv ("QS_SYSMON_SWAPS", missing, TRUE);
  empty = sysmon_read_swaps ();
  g_assert_nonnull (empty);
  g_assert_cmpuint (empty->len, ==, 0);
  sysmon_free_disks (empty);

  g_setenv ("QS_SYSMON_MOUNTS", missing, TRUE);
  no_mounts = sysmon_read_disks ();
  g_assert_nonnull (no_mounts);
  g_assert_cmpuint (no_mounts->len, ==, 0);
  sysmon_free_disks (no_mounts);
  key = sysmon_mounts_key ();
  g_assert_null (key);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (missing);
  g_free (swaps);
}

/* (f) zram NÃO é partição (RAM comprimida): filtrado da soma swap.
 * Fixture com zram0 ~31 GiB + partição ~36 GiB → linha "swap" soma
 * SÓ a partição; zram sozinho → sem linha swap. D-011. */
static void
test_zram_ignored (void)
{
  gchar *mounts;
  gchar *swaps;
  GPtrArray *sw;
  GPtrArray *disks;
  GPtrArray *only_zram;
  GPtrArray *disks_zram_only;
  SysmonDisk *one;
  SysmonDisk *in_disks;
  gdouble expected;
  gdouble diff;

  mounts = write_fixture ("nvme-mounts-zram",
                          "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
                          "/dev/nvme0n1p2 / ext4 rw 0 0\n");
  swaps = write_fixture ("nvme-swaps-zram",
                         "Filename Type Size Used Priority\n"
                         "/dev/zram0 partition 32505856 8388608 100\n"
                         "/dev/nvme0n1p3 partition 37748736 1048576 -2\n");
  g_setenv ("QS_SYSMON_MOUNTS", mounts, TRUE);
  g_setenv ("QS_SYSMON_SWAPS", swaps, TRUE);

  sw = sysmon_read_swaps ();
  g_assert_nonnull (sw);
  g_assert_cmpuint (sw->len, ==, 1);
  one = g_ptr_array_index (sw, 0);
  g_assert_cmpstr (one->name, ==, "swap");
  g_assert_cmpstr (one->text, ==, "1.00 GB / 36.00 GB");
  g_assert_null (strstr (one->text, "67"));
  expected = (gdouble) 1048576 / (gdouble) 37748736;
  diff = one->fraction - expected;
  if (diff < 0)
    diff = -diff;
  g_assert_true (diff < 1e-9);
  check_disk_sane (one);

  disks = sysmon_read_disks ();
  g_assert_nonnull (disks);
  in_disks = find_disk (disks, "swap");
  g_assert_nonnull (in_disks);
  g_assert_cmpstr (in_disks->text, ==, "1.00 GB / 36.00 GB");
  sysmon_free_disks (disks);
  sysmon_free_disks (sw);

  g_assert_true (g_file_set_contents (swaps,
                                      "Filename Type Size Used Priority\n"
                                      "/dev/zram0 partition 32505856 8388608 100\n",
                                      -1, NULL));
  only_zram = sysmon_read_swaps ();
  g_assert_nonnull (only_zram);
  g_assert_cmpuint (only_zram->len, ==, 0);
  sysmon_free_disks (only_zram);

  disks_zram_only = sysmon_read_disks ();
  g_assert_nonnull (disks_zram_only);
  g_assert_null (find_disk (disks_zram_only, "swap"));
  sysmon_free_disks (disks_zram_only);

  g_unsetenv ("QS_SYSMON_MOUNTS");
  g_unsetenv ("QS_SYSMON_SWAPS");
  g_free (mounts);
  g_free (swaps);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-sysmon-nvme-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);

  g_test_add_func ("/sysmon-nvme/swap_line", test_swap_line);
  g_test_add_func ("/sysmon-nvme/efi_small_formats_mb",
                   test_efi_small_formats_mb);
  g_test_add_func ("/sysmon-nvme/mounts_key_changes_with_swap",
                   test_mounts_key_changes_with_swap);
  g_test_add_func ("/sysmon-nvme/statvfs_fail_skipped",
                   test_statvfs_fail_skipped);
  g_test_add_func ("/sysmon-nvme/efivarfs_filtered",
                   test_efivarfs_filtered);
  g_test_add_func ("/sysmon-nvme/small_hidden", test_small_hidden);
  g_test_add_func ("/sysmon-nvme/swaps_robust", test_swaps_robust);
  g_test_add_func ("/sysmon-nvme/zram_ignored", test_zram_ignored);
  return g_test_run ();
}
