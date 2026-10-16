/* test-ui.c — testes estruturais das páginas/seções de UI (grupo UI).
 *
 * Constrói cada página via builder público e valida estrutura: páginas no
 * stack, tiles, widgets-chave, textos iniciais, ranges de sliders, presets
 * de nightlight, botões de sessão. Sem sinais (só estrutura + navegação de
 * stack); o comportamento dirigido por eventos vive em test-e2e.c.
 *
 * Precisa de display (widgets reais nunca mapeados): pula tudo sem display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-core.h"
#include "qs-net.h"
#include "qs-audio.h"
#include "qs-display.h"
#include "qs-ui.h"
#include "qs-power.h"
#include "qs-notify.h"
#include "qs-notify-store.h"
#include "qs-session.h"
#include "qs-sysmon.h"

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;

static QuickSettings *
qs_new (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs_ref (qs);
  return qs;
}

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

/* Conta filhos diretos de um container. */
static guint
n_children (GtkWidget *w)
{
  GList *kids = gtk_container_get_children (GTK_CONTAINER (w));
  guint  n = g_list_length (kids);

  g_list_free (kids);
  return n;
}

/* A página `name` existe no stack? */
static void
assert_page_exists (GtkWidget *stack, const gchar *name)
{
  GtkWidget *page = gtk_stack_get_child_by_name (GTK_STACK (stack), name);

  g_assert_nonnull (page);
}

/* --- wifi --- */

static void
test_wifi_page_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *stack;
  GtkWidget     *back = NULL;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  stack = gtk_stack_new ();
  wifi_build_page (stack, qs, &back);
  /* net_build_section (não chamada aqui) é quem cria o tile; handlers exigem. */
  qs->wifi_tile = gtk_button_new ();

  assert_page_exists (stack, "wifi");
  g_assert_true (GTK_IS_SWITCH (qs->wifi_switch));
  g_assert_true (GTK_IS_LIST_BOX (qs->wifi_list));
  g_assert_nonnull (back);
  g_assert_nonnull (qs->wifi_status);

  qs_unref (qs);
}

/* --- bluetooth --- */

static void
test_bt_page_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *stack;
  GtkWidget     *back = NULL;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  stack = gtk_stack_new ();
  bt_build_page (stack, qs, &back);
  qs->bt_tile = gtk_button_new ();

  assert_page_exists (stack, "bt");
  g_assert_true (GTK_IS_SWITCH (qs->bt_switch));
  g_assert_true (GTK_IS_LIST_BOX (qs->bt_list));
  g_assert_nonnull (back);
  g_assert_nonnull (qs->bt_status);

  qs_unref (qs);
}

/* --- audio --- */

static void
test_audio_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;
  gdouble        min = 0.0;
  gdouble        max = 0.0;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);

  assert_page_exists (stack, "audio");
  g_assert_true (GTK_IS_SCALE (qs->vol_scale));
  {
    GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (qs->vol_scale));
    min = gtk_adjustment_get_lower (adj);
    max = gtk_adjustment_get_upper (adj);
  }
  g_assert_cmpfloat (min, ==, 0.0);
  g_assert_cmpfloat (max, ==, 150.0);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->audio_sub)), ==, "…");
  g_assert_true (GTK_IS_COMBO_BOX_TEXT (qs->sink_combo));
  g_assert_true (GTK_IS_COMBO_BOX_TEXT (qs->source_combo));

  qs_unref (qs);
}

/* --- display --- */

static void
test_display_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;
  gdouble        min = 0.0;
  gdouble        max = 0.0;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
  /* Deixa um eventual display_load_async assentar. */
  run_loop_ms (800);

  assert_page_exists (stack, "display");
  assert_page_exists (stack, "night");
  g_assert_true (GTK_IS_SCALE (qs->bright_scale));
  {
    GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (qs->bright_scale));
    min = gtk_adjustment_get_lower (adj);
    max = gtk_adjustment_get_upper (adj);
  }
  g_assert_cmpfloat (min, ==, (gdouble) BRIGHTNESS_MIN);
  g_assert_cmpfloat (max, ==, 100.0);
  g_assert_true (GTK_IS_COMBO_BOX_TEXT (qs->mon_combo));
  g_assert_true (GTK_IS_COMBO_BOX_TEXT (qs->mode_combo));
  g_assert_true (GTK_IS_COMBO_BOX_TEXT (qs->scale_combo));
  g_assert_false (gtk_widget_is_sensitive (qs->mon_switch));
  g_assert_nonnull (qs->display_status);

  qs_unref (qs);
}

/* --- sysmon --- */

static void
test_sysmon_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->stack = gtk_stack_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = qs->stack;
  sysmon_build_section (box, stack, qs);

  assert_page_exists (stack, "sysmon");
  g_assert_true (n_children (box) >= 1);
  g_assert_nonnull (qs->sysmon_ram_value);
  g_assert_nonnull (qs->sysmon_cpu_value);
  g_assert_nonnull (qs->sysmon_disk_box);

  qs_unref (qs);
}

/* --- notify --- */

static void
test_notify_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  notify_build_section (box, stack, qs);

  assert_page_exists (stack, "notices");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "Empty");
  g_assert_nonnull (qs->notify_list);
  g_assert_nonnull (qs->notify_clear);

  qs_unref (qs);
}

/* Acha um botão com qs-action == action na árvore. */
static GtkWidget *
find_button_by_action (GtkWidget *root, const gchar *action)
{
  if (GTK_IS_BUTTON (root))
    {
      const gchar *a = g_object_get_data (G_OBJECT (root), "qs-action");

      if (g_strcmp0 (a, action) == 0)
        return root;
    }
  if (GTK_IS_CONTAINER (root))
    {
      GList   *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList   *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL; l = l->next)
        {
          hit = find_button_by_action (GTK_WIDGET (l->data), action);
          if (hit != NULL)
            break;
        }
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

/* --- session --- */

static void
test_session_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;
  const gchar   *actions[] = { "halt", "reboot", "hibernate", "suspend", "logout" };
  guint          i;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  session_build_section (box, stack, qs);

  assert_page_exists (stack, "power");
  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    g_assert_nonnull (find_button_by_action (stack, actions[i]));

  qs_unref (qs);
}

/* --- power --- */

static void
test_power_section_structure (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  power_build_section (box, qs);

  g_assert_true (GTK_IS_WIDGET (qs->bat_box));
  g_assert_true (GTK_IS_IMAGE (qs->bat_icon));
  g_assert_true (GTK_IS_LABEL (qs->bat_label));
  g_assert_true (GTK_IS_LABEL (qs->bat_status));

  qs_unref (qs);
}

/* --- navegação --- */

static void
test_navigation_show_main (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *stack;
  GtkWidget     *main_box;
  GtkWidget     *back = NULL;
  const gchar   *name;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_NONE);
  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (stack), main_box, "main");
  wifi_build_page (stack, qs, &back);
  qs->wifi_tile = gtk_button_new ();
  qs->stack = stack;

  /* GtkStack só aceita visible-child em filho com flag visible: mostra tudo
     (sem toplevel, sem flicker) antes dos sets. */
  gtk_widget_show_all (stack);
  gtk_stack_set_visible_child_name (GTK_STACK (stack), "wifi");
  show_main_page (qs);
  name = gtk_stack_get_visible_child_name (GTK_STACK (stack));
  g_assert_cmpstr (name, ==, "main");

  qs_unref (qs);
}

/* --- nightlight presets --- */

static void
test_night_preset_buttons (void)
{
  QuickSettings *qs = qs_new ();
  GtkWidget     *box;
  GtkWidget     *stack;
  GtkWidget     *night_page;
  GList         *toggles = NULL;
  GList         *kids;
  GList         *l;
  const gchar   *temps[] = { "6500K", "5500K", "4500K", "3500K" };
  guint          i;

  if (skip_no_display ())
    {
      qs_unref (qs);
      return;
    }
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  display_build_section (box, stack, qs);
  run_loop_ms (800);

  night_page = gtk_stack_get_child_by_name (GTK_STACK (stack), "night");
  g_assert_nonnull (night_page);
  kids = gtk_container_get_children (GTK_CONTAINER (night_page));
  /* Coleta toggles recursivamente via container walk simples (1 nível de
   * preset_box + page): varre tudo. */
  for (l = kids; l != NULL; l = l->next)
    {
      GtkWidget *w = GTK_WIDGET (l->data);

      if (GTK_IS_TOGGLE_BUTTON (w))
        {
          toggles = g_list_prepend (toggles, w);
        }
      else if (GTK_IS_CONTAINER (w))
        {
          GList *sub = gtk_container_get_children (GTK_CONTAINER (w));
          GList *s;

          for (s = sub; s != NULL; s = s->next)
            if (GTK_IS_TOGGLE_BUTTON (s->data))
              toggles = g_list_prepend (toggles, s->data);
          g_list_free (sub);
        }
    }
  g_list_free (kids);
  g_assert_cmpuint (g_list_length (toggles), ==, 4);
  for (i = 0; i < G_N_ELEMENTS (temps); i++)
    {
      gboolean found = FALSE;

      for (l = toggles; l != NULL; l = l->next)
        {
          const gchar *t = g_object_get_data (G_OBJECT (l->data), "qs-temp");

          if (g_strcmp0 (t, temps[i]) == 0)
            {
              found = TRUE;
              break;
            }
        }
      g_assert_true (found);
    }
  g_list_free (toggles);

  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-ui-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);

  g_test_add_func ("/ui/wifi_page_structure", test_wifi_page_structure);
  g_test_add_func ("/ui/bt_page_structure", test_bt_page_structure);
  g_test_add_func ("/ui/audio_section_structure", test_audio_section_structure);
  g_test_add_func ("/ui/display_section_structure", test_display_section_structure);
  g_test_add_func ("/ui/sysmon_section_structure", test_sysmon_section_structure);
  g_test_add_func ("/ui/notify_section_structure", test_notify_section_structure);
  g_test_add_func ("/ui/session_section_structure", test_session_section_structure);
  g_test_add_func ("/ui/power_section_structure", test_power_section_structure);
  g_test_add_func ("/ui/navigation_show_main", test_navigation_show_main);
  g_test_add_func ("/ui/night_preset_buttons", test_night_preset_buttons);

  return g_test_run ();
}
