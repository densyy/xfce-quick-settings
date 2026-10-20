/* test-sysmon-flow.c — fluxo da página System (qs-sysmon.c).
 *
 * Cobre clique no tile (refresh + timer + navegação), tick de ~2s pelo main
 * loop (RAM/CPU/barras e reconstrução de discos), voltar para "main",
 * hide do popup parando o timer e sysmon_stop idempotente. Lê apenas /proc
 * e /sys reais (somente leitura), sem ferramentas externas.
 *
 * Precisa de display (widgets reais nunca mapeados): pula sem display.
 */
#include <glib.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-sysmon.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *tile;
  GtkWidget     *page;
  GtkWidget     *back;
} SysmonFixture;

static QuickSettings *
qs_new (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs_ref (qs);
  return qs;
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

static GtkWidget *
first_child (GtkWidget *widget)
{
  GList     *kids = gtk_container_get_children (GTK_CONTAINER (widget));
  GtkWidget *child = NULL;

  if (kids != NULL)
    child = GTK_WIDGET (kids->data);
  g_list_free (kids);
  return child;
}

typedef gboolean (*WaitPredicate) (gpointer data);

/* Itera o main context em fatias curtas até a condição virar verdadeira.
 * Timers de segundos do GLib podem atrasar ~1s (coalescing) e mais sob
 * carga, então o timeout é generoso e não depende do instante exato. */
static gboolean
wait_until (WaitPredicate predicate,
            gpointer      data,
            guint         timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;

  while (!predicate (data))
    {
      if (g_get_monotonic_time () >= deadline)
        return FALSE;
      g_main_context_iteration (NULL, FALSE);
      g_usleep (10 * 1000);
    }
  return TRUE;
}

typedef struct
{
  SysmonFixture *f;
  GtkWidget     *child_before;
} TickWait;

static gboolean
tick_rendered (gpointer data)
{
  TickWait *w = data;
  gdouble   fraction;

  fraction = gtk_progress_bar_get_fraction (
               GTK_PROGRESS_BAR (w->f->qs->sysmon_cpu_bar));
  return w->f->qs->sysmon_disk_key != NULL
         && first_child (w->f->qs->sysmon_disk_box) != w->child_before
         && fraction >= 0.0;
}

typedef struct
{
  SysmonFixture *f;
  GtkWidget     *child;
  GtkWidget     *pct;
} CachedTickWait;

/* O caminho cacheado atualiza incondicionalmente o texto/barra de cada
 * linha de disco. Alterar o % de uma linha para um sentinela torna o
 * próximo tick observável sem depender de RAM/CPU mudarem. */
static gboolean
cached_tick_rendered (gpointer data)
{
  CachedTickWait *w = data;

  if (first_child (w->f->qs->sysmon_disk_box) != w->child)
    return FALSE;
  return g_strcmp0 (gtk_label_get_text (GTK_LABEL (w->pct)),
                    "waiting") != 0;
}

static gboolean
timer_cleared (gpointer data)
{
  SysmonFixture *f = data;

  return f->qs->sysmon_timer == 0;
}

static void
fixture_init (SysmonFixture *f)
{
  GtkWidget *main_box;
  GtkWidget *header;

  f->qs = qs_new ();
  f->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  f->stack = gtk_stack_new ();
  f->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->qs->popup = f->popup;
  f->qs->stack = f->stack;
  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (f->stack), main_box, "main");
  sysmon_build_section (f->box, f->stack, f->qs);

  f->tile = first_child (f->box);
  g_assert_nonnull (f->tile);
  f->page = gtk_stack_get_child_by_name (GTK_STACK (f->stack), "sysmon");
  g_assert_nonnull (f->page);
  header = first_child (f->page);
  g_assert_nonnull (header);
  f->back = first_child (header);
  g_assert_nonnull (f->back);
  /* GtkStack só aceita visible-child em filho com flag visible. */
  gtk_widget_show_all (f->stack);
}

static void
fixture_clear (SysmonFixture *f)
{
  sysmon_stop (f->qs);
  gtk_widget_destroy (f->box);
  gtk_widget_destroy (f->stack);
  gtk_widget_destroy (f->popup);
  f->qs->popup = NULL;
  f->qs->stack = NULL;
  qs_unref (f->qs);
}

static void
test_tile_tick_back (void)
{
  SysmonFixture *f;
  GtkWidget     *disk_child_before;
  GtkWidget     *disk_child_after;
  GtkWidget     *disk_pct;
  TickWait       tick_wait;
  CachedTickWait cached_wait;
  gchar         *ram_text;
  gchar         *cpu_value;
  gdouble        ram_fraction;
  gdouble        cpu_fraction;

  if (skip_no_display ())
    return;
  f = g_new0 (SysmonFixture, 1);
  fixture_init (f);

  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f->stack)),
                   ==, "main");
  sysmon_stop (f->qs);
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);

  g_signal_emit_by_name (f->tile, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f->stack)),
                   ==, "sysmon");
  g_assert_cmpuint (f->qs->sysmon_timer, !=, 0);

  ram_text = g_strdup (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_ram_value)));
  g_assert_nonnull (strstr (ram_text, "GB"));
  g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_ram_pct)),
                            "%"));
  ram_fraction = gtk_progress_bar_get_fraction (GTK_PROGRESS_BAR (f->qs->sysmon_ram_bar));
  g_assert_cmpfloat (ram_fraction, >=, 0.0);
  g_assert_cmpfloat (ram_fraction, <=, 1.0);

  cpu_value = g_strdup (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_cpu_value)));
  g_assert_true (strstr (cpu_value, "thread") != NULL
                 || strstr (cpu_value, "cpu") != NULL);
  g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_cpu_pct)),
                            "%"));
  cpu_fraction = gtk_progress_bar_get_fraction (GTK_PROGRESS_BAR (f->qs->sysmon_cpu_bar));
  g_assert_cmpfloat (cpu_fraction, >=, 0.0);
  g_assert_cmpfloat (cpu_fraction, <=, 1.0);

  g_assert_nonnull (first_child (f->qs->sysmon_disk_box));

  /* Invalida o hash de discos e o delta de CPU: o tick de 2s precisa
   * reconstruir as linhas e recalcular a barra, não só rearmar. */
  g_free (f->qs->sysmon_disk_key);
  f->qs->sysmon_disk_key = NULL;
  f->qs->sysmon_have_prev = FALSE;
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (f->qs->sysmon_cpu_bar), -0.5);
  disk_child_before = first_child (f->qs->sysmon_disk_box);
  tick_wait.f = f;
  tick_wait.child_before = disk_child_before;

  g_assert_true (wait_until (tick_rendered, &tick_wait, 8000));

  g_assert_cmpuint (f->qs->sysmon_timer, !=, 0);
  g_assert_nonnull (f->qs->sysmon_disk_key);
  g_assert_true (first_child (f->qs->sysmon_disk_box) != disk_child_before);
  g_assert_cmpfloat (gtk_progress_bar_get_fraction (GTK_PROGRESS_BAR (f->qs->sysmon_cpu_bar)),
                     >=, 0.0);
  g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_ram_pct)),
                            "%"));
  g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_cpu_pct)),
                            "%"));

  /* Próximo tick com o mesmo hash de mounts: mantém as linhas de disco
   * existentes (caminho cacheado, sem statvfs). O sentinela no % de uma
   * linha só é restaurado pelo próximo tick. */
  disk_child_after = first_child (f->qs->sysmon_disk_box);
  g_assert_nonnull (disk_child_after);
  disk_pct = g_object_get_data (G_OBJECT (disk_child_after), "qs-sys-pct");
  g_assert_nonnull (disk_pct);
  gtk_label_set_text (GTK_LABEL (disk_pct), "waiting");
  cached_wait.f = f;
  cached_wait.child = disk_child_after;
  cached_wait.pct = disk_pct;

  g_assert_true (wait_until (cached_tick_rendered, &cached_wait, 8000));
  g_assert_cmpuint (f->qs->sysmon_timer, !=, 0);
  g_assert_true (first_child (f->qs->sysmon_disk_box) == disk_child_after);
  g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (f->qs->sysmon_ram_pct)),
                            "%"));

  g_signal_emit_by_name (f->back, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f->stack)),
                   ==, "main");
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);
  sysmon_stop (f->qs);
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);

  g_free (cpu_value);
  g_free (ram_text);
  fixture_clear (f);
  g_free (f);
}

static void
test_tile_twice_and_hide (void)
{
  SysmonFixture *f;
  guint          timer;

  if (skip_no_display ())
    return;
  f = g_new0 (SysmonFixture, 1);
  fixture_init (f);

  g_signal_emit_by_name (f->tile, "clicked");
  timer = f->qs->sysmon_timer;
  g_assert_cmpuint (timer, !=, 0);

  /* Segundo clique atualiza valores mas reusa o timer existente. */
  g_signal_emit_by_name (f->tile, "clicked");
  g_assert_cmpuint (f->qs->sysmon_timer, ==, timer);

  /* hide do popup para o timer (e é idempotente). */
  g_signal_emit_by_name (f->popup, "hide");
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);
  sysmon_stop (f->qs);
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);

  fixture_clear (f);
  g_free (f);
}

static void
test_dead_tick_stops (void)
{
  SysmonFixture *f;

  if (skip_no_display ())
    return;
  f = g_new0 (SysmonFixture, 1);
  fixture_init (f);

  g_signal_emit_by_name (f->tile, "clicked");
  g_assert_cmpuint (f->qs->sysmon_timer, !=, 0);

  /* O próximo tick vê qs->dead e remove a fonte sozinho. */
  f->qs->dead = TRUE;
  g_assert_true (wait_until (timer_cleared, f, 8000));
  g_assert_cmpuint (f->qs->sysmon_timer, ==, 0);

  f->qs->dead = FALSE;
  fixture_clear (f);
  g_free (f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-sysmon-flow-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/sysmon-flow/tile_tick_back", test_tile_tick_back);
  g_test_add_func ("/sysmon-flow/tile_twice_and_hide",
                   test_tile_twice_and_hide);
  g_test_add_func ("/sysmon-flow/dead_tick_stops", test_dead_tick_stops);

  return g_test_run ();
}
