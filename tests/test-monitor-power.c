/* test-monitor-power.c — fluxos de monitores (qs-monitor.c).
 *
 * Cobre a página Display com xrandr falso: indisponível (sem xrandr),
 * query vazia/sem monitores/erro, preenchimento dos combos, troca de
 * monitor/modo/escala, power on/off, guarda de display único e falha de
 * spawn com reversão do switch.
 *
 * PATH é sempre restrito a um diretório temporário com o fake xrandr;
 * nenhum xrandr real é executado e o QS_FAKE_LOG prova os argv chamados.
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-display.h"
#include "qs-util.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fake_xrandr = NULL;
static guint    fixture_seq = 0;

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *popup;
  gchar         *bin;
  gchar         *log;
} MonFixture;

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

/* run_command_async ignora o exit status (test-async /async/exit_ignored):
 * um xrandr que sai != 0 ainda chega como ok=TRUE. O ramo de falha de
 * mon_power_done só é alcançado quando o próprio spawn falha; o shebang
 * sem interpretador mantém command_exists TRUE e produz ok=FALSE. */
static void
break_spawn (const gchar *bin)
{
  gchar *dst = g_build_filename (bin, "xrandr", NULL);

  g_assert_true (g_file_set_contents (dst,
                                      "#!/nonexistent-qs-interpreter\n"
                                      "exit 1\n", -1, NULL));
  g_assert_cmpint (g_chmod (dst, 0755), ==, 0);
  g_free (dst);
}

static void
fixture_init (MonFixture *f,
              gboolean    with_xrandr)
{
  gchar *name;

  memset (f, 0, sizeof (*f));
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_QUERY_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_EMPTY");
  g_unsetenv ("QS_FAKE_XRANDR_SINGLE");

  f->bin = g_dir_make_tmp ("qs-monitor-bin-XXXXXX", NULL);
  g_assert_nonnull (f->bin);
  if (with_xrandr)
    copy_fake (f->bin);
  g_setenv ("PATH", f->bin, TRUE);

  name = g_strdup_printf ("monitor-%u.log", fixture_seq++);
  f->log = g_build_filename (tmp_root, name, NULL);
  g_free (name);
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
fixture_free (MonFixture *f)
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
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_QUERY_FAIL");
  g_unsetenv ("QS_FAKE_XRANDR_EMPTY");
  g_unsetenv ("QS_FAKE_XRANDR_SINGLE");
}

static gboolean
log_has (const MonFixture *f,
         const gchar      *needle)
{
  gchar   *text = NULL;
  gboolean hit;

  g_file_get_contents (f->log, &text, NULL, NULL);
  hit = (text != NULL && strstr (text, needle) != NULL);
  g_free (text);
  return hit;
}

static guint
log_count (const MonFixture *f,
           const gchar      *needle)
{
  gchar   *text = NULL;
  gchar   *p;
  guint    n = 0;
  gsize    len = strlen (needle);

  g_file_get_contents (f->log, &text, NULL, NULL);
  if (text == NULL)
    return 0;
  for (p = text; (p = strstr (p, needle)) != NULL; p += len)
    n++;
  g_free (text);
  return n;
}

static void
log_clear (const MonFixture *f)
{
  g_assert_true (g_file_set_contents (f->log, "", 0, NULL));
}

static gboolean
wait_log (const MonFixture *f,
          const gchar      *needle,
          guint             timeout_ms)
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

static gboolean
wait_label (GtkWidget   *label,
            const gchar *needle,
            guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      const gchar *text = gtk_label_get_text (GTK_LABEL (label));

      if (g_strcmp0 (text, needle) == 0)
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), needle) == 0;
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

static void
assert_combo_text (GtkWidget   *combo,
                   const gchar *expected)
{
  gchar *text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (combo));

  g_assert_cmpstr (text, ==, expected);
  g_free (text);
}

static void
assert_unavailable_combos (QuickSettings *qs)
{
  assert_combo_text (qs->mon_combo, "unavailable");
  assert_combo_text (qs->mode_combo, "unavailable");
  assert_combo_text (qs->scale_combo, "unavailable");
  g_assert_false (gtk_widget_is_sensitive (qs->mon_combo));
  g_assert_false (gtk_widget_is_sensitive (qs->mode_combo));
  g_assert_false (gtk_widget_is_sensitive (qs->scale_combo));
}

/* --- sem xrandr --- */

static void
test_unavailable (void)
{
  MonFixture f;

  if (skip_no_display ())
    return;
  fixture_init (&f, FALSE);
  g_assert_false (command_exists ("xrandr"));

  display_refresh_tile (f.qs);

  assert_unavailable_combos (f.qs);
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_false (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_status)), ==,
                   "xrandr not found (unavailable).");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "unavailable");
  g_assert_false (gtk_widget_is_sensitive (f.qs->display_tile));

  /* display_load_async direto também cai no caminho indisponível. */
  display_load_async (f.qs);
  assert_unavailable_combos (f.qs);

  fixture_free (&f);
}

/* --- erros de query e query sem monitores --- */

static void
test_query_errors (void)
{
  MonFixture f;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);

  /* Saída vazia + exit != 0: combos "No displays". */
  g_setenv ("QS_FAKE_XRANDR_QUERY_FAIL", "1", TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_label (f.qs->display_status,
                             "Could not query displays.", 3000));
  assert_combo_text (f.qs->mon_combo, "No displays");
  assert_combo_text (f.qs->mode_combo, "No displays");
  assert_combo_text (f.qs->scale_combo, "No displays");
  g_assert_false (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "No displays");
  g_unsetenv ("QS_FAKE_XRANDR_QUERY_FAIL");

  /* QS_FAKE_XRANDR_FAIL global também zera o stdout da query. */
  g_setenv ("QS_FAKE_XRANDR_FAIL", "1", TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_label (f.qs->display_status,
                             "Could not query displays.", 3000));
  g_unsetenv ("QS_FAKE_XRANDR_FAIL");

  /* Spawn falho (ok=FALSE): mesmo status de erro. */
  break_spawn (f.bin);
  g_assert_true (command_exists ("xrandr"));
  display_refresh_tile (f.qs);
  g_assert_true (wait_label (f.qs->display_status,
                             "Could not query displays.", 3000));
  g_assert_cmpint (f.qs->refcount, ==, 1);
  copy_fake (f.bin);

  /* Dump sem outputs conectados: "No connected displays.". */
  g_setenv ("QS_FAKE_XRANDR_EMPTY", "1", TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_label (f.qs->display_status,
                             "No connected displays.", 3000));
  assert_combo_text (f.qs->mon_combo, "No displays");
  g_assert_false (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "No displays");
  g_assert_true (gtk_widget_is_sensitive (f.qs->display_tile));
  g_unsetenv ("QS_FAKE_XRANDR_EMPTY");

  /* Combo em "none" e depois query normal: prev vira NULL. */
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  /* Prev válido mas ausente do novo dump: cai no primeiro monitor. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-1"));
  run_loop_ms (50);
  g_setenv ("QS_FAKE_XRANDR_SINGLE", "1", TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  assert_combo_text (f.qs->mode_combo, "1920x1080");
  g_unsetenv ("QS_FAKE_XRANDR_SINGLE");

  fixture_free (&f);
}

/* --- carga, seleção e guards dos handlers --- */

static void
test_load_and_select (void)
{
  MonFixture f;
  gchar       *mode_before;
  gchar       *saved_last;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  g_assert_true (gtk_widget_is_sensitive (f.qs->display_tile));
  g_assert_true (gtk_widget_is_sensitive (f.qs->mon_combo));
  assert_combo_text (f.qs->mode_combo, "1920x1080");
  assert_combo_text (f.qs->scale_combo, "1x");
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_true (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "HDMI-1 · 1920x1080");

  /* Guard: updating. */
  mode_before = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (f.qs->mode_combo)));
  f.qs->updating = TRUE;
  on_mon_changed (GTK_COMBO_BOX (f.qs->mon_combo), f.qs);
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (f.qs->mode_combo)),
                   ==, mode_before);
  f.qs->updating = FALSE;

  /* Guard: combo insensível. */
  gtk_widget_set_sensitive (f.qs->mon_combo, FALSE);
  on_mon_changed (GTK_COMBO_BOX (f.qs->mon_combo), f.qs);
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (f.qs->mode_combo)),
                   ==, mode_before);
  gtk_widget_set_sensitive (f.qs->mon_combo, TRUE);

  /* Guard: id inválido. */
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (f.qs->mon_combo),
                             "bad name", "bad name");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "bad name"));
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (f.qs->mode_combo)),
                   ==, mode_before);

  /* Guard: sem dump em cache. */
  saved_last = f.qs->display_last;
  f.qs->display_last = NULL;
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (f.qs->mon_combo),
                             "DP-9", "DP-9");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-9"));
  f.qs->display_last = saved_last;

  /* Monitor válido mas ausente do snapshot. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "HDMI-1"));
  run_loop_ms (50);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-9"));
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (f.qs->mode_combo)),
                   ==, mode_before);

  /* Troca real de monitor: modos do DP-1 e switch ligado. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-1"));
  g_assert_true (wait_combo (f.qs->mode_combo, "1280x1024", 1000));
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_true (g_str_has_prefix (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)),
                                   "DP-1 · "));

  /* Modo: caminho válido. */
  log_clear (&f);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mode_combo),
                                              "1024x768"));
  g_assert_true (wait_log (&f, "xrandr --output DP-1 --mode 1024x768", 3000));

  /* Modo: guards updating/insensível/mon inválido/modo inválido. */
  f.qs->updating = TRUE;
  log_clear (&f);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mode_combo),
                                              "1280x1024"));
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --mode "), ==, 0);
  f.qs->updating = FALSE;

  gtk_widget_set_sensitive (f.qs->mode_combo, FALSE);
  log_clear (&f);
  on_mode_changed (GTK_COMBO_BOX (f.qs->mode_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --mode "), ==, 0);
  gtk_widget_set_sensitive (f.qs->mode_combo, TRUE);

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (f.qs->mode_combo),
                             "bogus", "bogus");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mode_combo),
                                              "bogus"));
  log_clear (&f);
  on_mode_changed (GTK_COMBO_BOX (f.qs->mode_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --mode "), ==, 0);

  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "bad name"));
  log_clear (&f);
  on_mode_changed (GTK_COMBO_BOX (f.qs->mode_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --mode "), ==, 0);

  /* Escala: caminho válido. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DP-1"));
  run_loop_ms (50);
  log_clear (&f);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                              "1.5"));
  g_assert_true (wait_log (&f, "xrandr --output DP-1 --scale 1.5x1.5", 3000));

  /* Escala: guards updating/insensível/fator inválido/mon inválido. */
  f.qs->updating = TRUE;
  log_clear (&f);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                              "2"));
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --scale "), ==, 0);
  f.qs->updating = FALSE;

  gtk_widget_set_sensitive (f.qs->scale_combo, FALSE);
  log_clear (&f);
  on_scale_changed (GTK_COMBO_BOX (f.qs->scale_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --scale "), ==, 0);
  gtk_widget_set_sensitive (f.qs->scale_combo, TRUE);

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (f.qs->scale_combo),
                             "3", "3x");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->scale_combo),
                                              "3"));
  log_clear (&f);
  on_scale_changed (GTK_COMBO_BOX (f.qs->scale_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --scale "), ==, 0);

  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "bad name"));
  log_clear (&f);
  on_scale_changed (GTK_COMBO_BOX (f.qs->scale_combo), f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --scale "), ==, 0);

  g_free (mode_before);
  fixture_free (&f);
}

/* --- guarda de display único --- */

static void
test_single_display_guard (void)
{
  MonFixture f;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);
  g_setenv ("QS_FAKE_XRANDR_SINGLE", "1", TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));

  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_false (gtk_widget_is_sensitive (f.qs->mon_switch));
  {
    gchar *tip = gtk_widget_get_tooltip_text (f.qs->mon_switch);

    g_assert_cmpstr (tip, ==, "Cannot disable the only display");
    g_free (tip);
  }
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_status)), ==,
                   "Cannot disable the only display.");

  /* Força o switch sensível e tenta desligar: handler reverte sem --off. */
  gtk_widget_set_sensitive (f.qs->mon_switch, TRUE);
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  run_loop_ms (200);
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_false (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_status)), ==,
                   "Cannot disable the only display.");
  g_assert_false (log_has (&f, " --off"));

  /* VGA-1 conectado mas desligado e sem modos: pode religar. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "VGA-1"));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_status)), ==,
                   "");
  assert_combo_text (f.qs->mode_combo, "No modes");
  g_assert_false (gtk_widget_is_sensitive (f.qs->mode_combo));
  assert_combo_text (f.qs->scale_combo, "1x");
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_true (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "VGA-1 · Off");

  /* DVI-1 desligado mas com modos: fallback para o primeiro modo. */
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "DVI-1"));
  assert_combo_text (f.qs->mode_combo, "1600x900");
  g_assert_true (gtk_widget_is_sensitive (f.qs->mode_combo));
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_true (gtk_widget_is_sensitive (f.qs->mon_switch));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->display_sub)), ==,
                   "DVI-1 · Off");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "VGA-1"));

  /* Falha de spawn ao ligar: switch volta e status mostra o erro. */
  break_spawn (f.bin);
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), TRUE);
  g_assert_true (wait_label (f.qs->display_status,
                             "Failed to turn monitor on.", 3000));
  g_assert_false (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_false (log_has (&f, " --auto"));
  copy_fake (f.bin);

  /* Sucesso depois da falha: --auto e nova query de re-sincronização. */
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), TRUE);
  g_assert_true (wait_log (&f, "xrandr --output VGA-1 --auto", 3000));
  g_assert_true (wait_log (&f, "xrandr --query", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));
  g_unsetenv ("QS_FAKE_XRANDR_SINGLE");

  fixture_free (&f);
}

/* --- power on/off e guards do switch --- */

static void
test_power_cycle (void)
{
  MonFixture f;
  GtkWidget *saved_switch;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  /* Falha de spawn ao desligar: switch reverte e nada de --off. */
  break_spawn (f.bin);
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  g_assert_true (wait_label (f.qs->display_status,
                             "Failed to turn monitor off.", 3000));
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));
  g_assert_false (log_has (&f, " --off"));
  copy_fake (f.bin);

  /* Desliga: --off seguido de --query. Fake ainda reporta ativo, então o
   * switch re-sincroniza ligado. */
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --off", 3000));
  g_assert_true (wait_log (&f, "xrandr --query", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));
  g_assert_true (gtk_switch_get_active (GTK_SWITCH (f.qs->mon_switch)));

  /* Liga: --auto seguido de --query (toggle off/on imediato para o switch
   * ainda ativo após a re-sincronização). */
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), TRUE);
  g_assert_true (wait_log (&f, "xrandr --output HDMI-1 --auto", 3000));
  g_assert_true (wait_log (&f, "xrandr --query", 3000));
  drain_async (f.qs);

  /* Guards: updating, switch insensível, switch NULL e monitor inválido. */
  f.qs->updating = TRUE;
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  run_loop_ms (150);
  g_assert_cmpuint (log_count (&f, " --off"), ==, 0);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), TRUE);
  f.qs->updating = FALSE;

  gtk_widget_set_sensitive (f.qs->mon_switch, FALSE);
  log_clear (&f);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  run_loop_ms (150);
  g_assert_cmpuint (log_count (&f, " --off"), ==, 0);
  gtk_widget_set_sensitive (f.qs->mon_switch, TRUE);
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), TRUE);
  run_loop_ms (50);

  saved_switch = f.qs->mon_switch;
  f.qs->mon_switch = NULL;
  log_clear (&f);
  on_mon_power_changed (G_OBJECT (saved_switch), NULL, f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --off"), ==, 0);
  f.qs->mon_switch = saved_switch;

  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (f.qs->mon_combo),
                             "bad name", "bad name");
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "bad name"));
  log_clear (&f);
  on_mon_power_changed (G_OBJECT (f.qs->mon_switch), NULL, f.qs);
  run_loop_ms (100);
  g_assert_cmpuint (log_count (&f, " --off"), ==, 0);
  g_assert_true (gtk_combo_box_set_active_id (GTK_COMBO_BOX (f.qs->mon_combo),
                                              "HDMI-1"));
  run_loop_ms (50);

  fixture_free (&f);
}

/* --- mon_switch ausente durante o preenchimento --- */

static void
test_power_switch_missing (void)
{
  MonFixture f;
  GtkWidget *saved_switch;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);
  saved_switch = f.qs->mon_switch;
  f.qs->mon_switch = NULL;

  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  f.qs->mon_switch = saved_switch;
  fixture_free (&f);
}

/* --- callbacks após free-data (dead) --- */

static void
test_dead_callbacks (void)
{
  MonFixture f;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);

  /* display_query_done vê dead: só libera a referência. */
  f.qs->dead = TRUE;
  display_refresh_tile (f.qs);
  drain_async (f.qs);
  g_assert_cmpint (f.qs->refcount, ==, 1);
  f.qs->dead = FALSE;
  f.qs->display_loading = FALSE;

  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  /* mon_power_done vê dead: só libera a referência. */
  f.qs->dead = TRUE;
  gtk_switch_set_active (GTK_SWITCH (f.qs->mon_switch), FALSE);
  drain_async (f.qs);
  g_assert_cmpint (f.qs->refcount, ==, 1);
  f.qs->dead = FALSE;

  fixture_free (&f);
}

/* --- guard de display_loading --- */

static void
test_loading_guard (void)
{
  MonFixture f;
  guint      before;

  if (skip_no_display ())
    return;
  fixture_init (&f, TRUE);
  display_refresh_tile (f.qs);
  g_assert_true (wait_combo (f.qs->mon_combo, "HDMI-1", 3000));
  g_assert_true (wait_label (f.qs->display_status, "", 3000));

  before = log_count (&f, "xrandr --query");
  f.qs->display_loading = TRUE;
  display_refresh_tile (f.qs);
  run_loop_ms (150);
  g_assert_cmpuint (log_count (&f, "xrandr --query"), ==, before);
  f.qs->display_loading = FALSE;

  fixture_free (&f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-monitor-power-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  {
    gchar *cwd = g_get_current_dir ();

    fake_xrandr = g_build_filename (cwd, "tests", "fakebin-xrandr",
                                    "xrandr", NULL);
    g_free (cwd);
  }
  g_assert_true (g_file_test (fake_xrandr, G_FILE_TEST_IS_REGULAR));

  g_test_add_func ("/monitor/unavailable", test_unavailable);
  g_test_add_func ("/monitor/query_errors", test_query_errors);
  g_test_add_func ("/monitor/load_and_select", test_load_and_select);
  g_test_add_func ("/monitor/single_display_guard", test_single_display_guard);
  g_test_add_func ("/monitor/power_cycle", test_power_cycle);
  g_test_add_func ("/monitor/power_switch_missing", test_power_switch_missing);
  g_test_add_func ("/monitor/dead_callbacks", test_dead_callbacks);
  g_test_add_func ("/monitor/loading_guard", test_loading_guard);

  {
    gint ret = g_test_run ();

    g_rmdir (tmp_root);
    g_free (tmp_root);
    g_free (fake_xrandr);
    return ret;
  }
}
