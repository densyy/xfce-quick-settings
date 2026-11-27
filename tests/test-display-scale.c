/* test-display-scale.c — regressão D-004 (escala invertida, qs-monitor.c) +
 * D-010 (escala persiste por monitor via state.ini: grupo `display`,
 * chave `scale_<output>`; restore em fill_scale_combo + reaplicação xrandr).
 *
 * Prova que on_scale_changed envia o inverso do fator XFCE via
 * xrandr --scale IxI (I = 1 / fator): 1.25 -> 0.8x0.8 e 1 -> 1x1, com a
 * tabela fixa completa (1.5 -> 0.667, 1.75 -> 0.571, 2 -> 0.5).
 *
 * D-010 acrescenta salva->restaura (inclui sair/voltar de tela via
 * mon_combo e fallback "1" para valor inválido) com cache isolado.
 *
 * PATH é restrito a um diretório temporário com o xrandr falso
 * (tests/fakebin-xrandr); nenhum xrandr real é executado e o
 * QS_FAKE_LOG prova os argv chamados. Precisa de display (widgets reais
 * nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-cache.h"
#include "qs-display.h"
#include "qs-util.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fake_xrandr = NULL;

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *popup;
  gchar         *bin;
  gchar         *log;
} ScaleFixture;

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static void
run_loop_ms (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static gboolean
skip_no_display (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return TRUE;
    }
  return FALSE;
}

static void
copy_fake (const gchar *bin)
{
  gchar *contents = NULL;
  gsize  len = 0;
  gchar *dst = g_build_filename (bin, "xrandr", NULL);

  g_assert_true (g_file_get_contents (fake_xrandr, &contents, &len, NULL));
  g_assert_true (g_file_set_contents (dst, contents, (gssize) len, NULL));
  g_assert_cmpint (g_chmod (dst, 0755), ==, 0);
  g_free (contents);
  g_free (dst);
}

static void
fixture_init (ScaleFixture *f)
{
  memset (f, 0, sizeof (*f));
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_QUERY_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_EMPTY");
  g_unsetenv ("QS_FAKE_XRANDR_SINGLE");

  f->bin = g_dir_make_tmp ("qs-scale-bin-XXXXXX", NULL);
  g_assert_nonnull (f->bin);
  copy_fake (f->bin);
  g_setenv ("PATH", f->bin, TRUE);

  f->log = g_build_filename (tmp_root, "scale-inverse.log", NULL);
  g_setenv ("QS_FAKE_LOG", f->log, TRUE);

  f->qs = g_new0 (QuickSettings, 1);
  qs_ref (f->qs);
  f->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  f->stack = gtk_stack_new ();
  f->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->qs->popup = f->popup;
  f->qs->stack = f->stack;
  display_build_section (f->box, f->stack, f->qs);
}

static void
drain_async (QuickSettings *qs)
{
  guint waited = 0;

  while (qs->refcount > 1 && waited < 3000)
    {
      run_loop_ms (20);
      waited += 20;
    }
}

static void
fixture_free (ScaleFixture *f)
{
  gchar *fake = g_build_filename (f->bin, "xrandr", NULL);

  drain_async (f->qs);
  g_assert_cmpint (f->qs->refcount, ==, 1);
  gtk_widget_destroy (f->box);
  gtk_widget_destroy (f->stack);
  gtk_widget_destroy (f->popup);
  qs_unref (f->qs);
  g_remove (fake);
  g_free (fake);
  g_remove (f->log);
  g_rmdir (f->bin);
  g_free (f->bin);
  g_free (f->log);
  g_unsetenv ("QS_FAKE_LOG");
}

static gboolean
log_has (const ScaleFixture *f,
         const gchar        *needle)
{
  gchar   *text = NULL;
  gboolean hit;

  g_file_get_contents (f->log, &text, NULL, NULL);
  hit = (text != NULL && strstr (text, needle) != NULL);
  g_free (text);
  return hit;
}

static gboolean
wait_log (const ScaleFixture *f,
          const gchar        *needle,
          guint               timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_has (f, needle))
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return log_has (f, needle);
}

static void
log_clear (const ScaleFixture *f)
{
  g_assert_true (g_file_set_contents (f->log, "", 0, NULL));
}

static gboolean
wait_combo (GtkWidget   *combo,
            const gchar *id,
            guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (g_strcmp0 (gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo)),
                     id) == 0)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return g_strcmp0 (gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo)),
                    id) == 0;
}

static gboolean
wait_label (GtkWidget   *label,
            const gchar *needle,
            guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), needle) == 0)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), needle) == 0;
}

/* D-010: o GLib cacheia o user cache dir no primeiro uso; o ini isolado
 * de cada teste vive em <tmp_root>/xfce4/quick-settings/state.ini. */
static gchar *
ini_path (void)
{
  return g_build_filename (tmp_root, "xfce4", "quick-settings",
                           "state.ini", NULL);
}

static void
assert_cache_isolated (void)
{
  const gchar *dir = g_get_user_cache_dir ();

  g_assert_nonnull (dir);
  g_assert_nonnull (tmp_root);
  g_assert_true (g_str_has_prefix (dir, tmp_root));
}

static gboolean
wait_ini_value (const gchar *key,
                const gchar *expected,
                guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      GKeyFile *kf = g_key_file_new ();
      gchar    *path = ini_path ();
      gchar    *val = NULL;
      gboolean  hit = FALSE;

      if (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
        val = g_key_file_get_string (kf, "display", key, NULL);
      hit = (g_strcmp0 (val, expected) == 0);
      g_free (val);
      g_free (path);
      g_key_file_free (kf);
      if (hit)
        return TRUE;
      run_loop_ms (50);
      waited += 50;
    }
  return FALSE;
}

/* D-004: cada fator XFCE gera o --scale inverso (tabela fixa). A ordem
 * começa fora do default "1" para que cada set_active_id dispare o
 * handler (troca de id); 1.25x e 1x são os casos exigidos pelo plano. */
static void
test_scale_inverse_table (void)
{
  ScaleFixture f;
  gsize        i;
  const gchar *factors[] = { "1.25", "1", "1.5", "1.75", "2" };
  const gchar *scales[] = { "0.8x0.8", "1x1", "0.667x0.667",
                            "0.571x0.571", "0.5x0.5" };

  if (skip_no_display ())
    return;
  fixture_init (&f);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  for (i = 0; i < G_N_ELEMENTS (factors); i++)
    {
      gchar *needle;

      log_clear (&f);
      g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                                 factors[i]));
      needle = g_strdup_printf ("xrandr --output HDMI-1 --scale %s",
                                scales[i]);
      g_assert_true (wait_log (&f, needle, 3000));
      g_free (needle);
    }

  fixture_free (&f);
}

/* D-010 salva->restaura: escolhe 1.5 no combo (xrandr 0.667 logado),
 * aguarda o debounce de qs_cache_schedule_save (500ms) gravar
 * display/scale_HDMI-1=1.5 no ini isolado e, com widgets novos sobre o
 * mesmo cache, prova que fill_scale_combo restaura o active id "1.5"
 * e que o xrandr de reaplicação roda de novo. */
static void
test_save_restore_persist (void)
{
  ScaleFixture f;
  gchar       *path;
  gchar       *cached;

  if (skip_no_display ())
    return;
  assert_cache_isolated ();
  path = ini_path ();
  g_remove (path);
  g_free (path);

  fixture_init (&f);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                              "1.5"));
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --scale 0.667x0.667",
                           3000));
  g_assert_true (wait_ini_value ("scale_HDMI-1", "1.5", 5000));
  cached = qs_cache_read_display_scale ("HDMI-1");
  g_assert_cmpstr (cached, ==, "1.5");
  g_free (cached);
  fixture_free (&f);

  /* Restaura: widgets novos, mesmo XDG_CACHE_HOME isolado. */
  fixture_init (&f);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_combo (f.qs->scale_combo, "1.5", 3000));
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --scale 0.667x0.667",
                           3000));
  fixture_free (&f);
}

/* D-010 sair->voltar da tela: com 1.75 salvo para HDMI-1, trocar o
 * mon_combo para DP-1 (sem escala salva: cai para o default "1") e
 * voltar para HDMI-1 preserva o active id "1.75" do cache, com
 * reaplicação xrandr (0.571x0.571) registrada no log. */
static void
test_leave_and_back_preserved (void)
{
  ScaleFixture f;
  gchar       *path;

  if (skip_no_display ())
    return;
  assert_cache_isolated ();
  path = ini_path ();
  g_remove (path);
  g_free (path);

  fixture_init (&f);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                              "1.75"));
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --scale 0.571x0.571",
                           3000));
  g_assert_true (wait_ini_value ("scale_HDMI-1", "1.75", 5000));

  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-1"));
  g_assert_true (wait_combo (f.qs->scale_combo, "1", 3000));

  log_clear (&f);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "HDMI-1"));
  g_assert_true (wait_combo (f.qs->scale_combo, "1.75", 3000));
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --scale 0.571x0.571",
                           3000));
  fixture_free (&f);
}

/* D-010 mutação de dados: valor inválido no cache ("bogus") é ignorado —
 * fill_scale_combo cai para "1" e nenhum xrandr --scale é reaplicado. */
static void
test_invalid_scale_fallback (void)
{
  ScaleFixture f;
  gchar       *path;
  gchar       *dir;
  gchar       *cached;

  if (skip_no_display ())
    return;
  assert_cache_isolated ();
  path = ini_path ();
  dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_free (dir);
  g_assert_true (g_file_set_contents (path, "[display]\nscale_HDMI-1=bogus\n",
                                      -1, NULL));
  g_free (path);

  cached = qs_cache_read_display_scale ("HDMI-1");
  g_assert_null (cached);

  fixture_init (&f);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_combo (f.qs->scale_combo, "1", 3000));
  run_loop_ms (300);
  g_assert_false (log_has (&f, "--scale"));
  fixture_free (&f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-display-scale-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* D-010: XDG ANTES de gtk_init_check — o GLib cacheia os dirs do
   * usuário no primeiro uso e o cache real (~/.cache) seria poluído. */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  g_reload_user_special_dirs_cache ();
  have_display = gtk_init_check (NULL, NULL);
  {
    gchar *cwd = g_get_current_dir ();

    fake_xrandr = g_build_filename (cwd, "tests", "fakebin-xrandr",
                                    "xrandr", NULL);
    g_free (cwd);
  }
  g_assert_true (g_file_test (fake_xrandr, G_FILE_TEST_IS_REGULAR));

  g_test_add_func ("/display-scale/inverse_table", test_scale_inverse_table);
  g_test_add_func ("/display-scale/save_restore_persist",
                   test_save_restore_persist);
  g_test_add_func ("/display-scale/leave_and_back_preserved",
                   test_leave_and_back_preserved);
  g_test_add_func ("/display-scale/invalid_scale_fallback",
                   test_invalid_scale_fallback);

  {
    gint ret = g_test_run ();

    g_rmdir (tmp_root);
    g_free (tmp_root);
    g_free (fake_xrandr);
    return ret;
  }
}
