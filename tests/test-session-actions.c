/* test-session-actions.c — ações de energia/sessão (qs-session.c).
 *
 * Constrói a seção Power (GtkOffscreenWindow + stack) e emite "clicked"
 * nos botões identificados por "qs-action". Dois caminhos:
 *  - PATH=tests/fakebin: xfce4-session-logout falso registra os argv;
 *    systemctl não existe nesse diretório, então nada real é chamado.
 *  - PATH=tests/fakebin-session: só systemctl/loginctl falsos e sem
 *    xfce4-session-logout, exercitando os fallbacks.
 * Antes de cada clique um guarda usa g_find_program_in_path e confere que
 * o executável resolvido está dentro do diretório fake esperado; fora
 * disso o teste aborta sem clicar.
 *
 * Precisa de display; a janela é GtkOffscreenWindow e nunca é mapeada.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "qs-core.h"
#include "qs-session.h"

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
} Fixture;

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;
static gchar   *fakebin = NULL;
static gchar   *fakebin_session = NULL;
static gchar   *log_path = NULL;

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

static gchar *
log_read (void)
{
  gchar *text = NULL;

  g_file_get_contents (log_path, &text, NULL, NULL);
  return text;
}

static guint
count_occurrences (const gchar *haystack,
                   const gchar *needle)
{
  const gchar *pos = haystack;
  gsize        len = strlen (needle);
  guint        count = 0;

  while ((pos = strstr (pos, needle)) != NULL)
    {
      count++;
      pos += len;
    }
  return count;
}

static gboolean
log_has (const gchar *needle)
{
  gchar   *text = log_read ();
  gboolean hit = (text != NULL && strstr (text, needle) != NULL);

  g_free (text);
  return hit;
}

static gboolean
wait_log (const gchar *needle,
          guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms)
    {
      if (log_has (needle))
        return TRUE;
      run_loop_ms (20);
      waited += 20;
    }
  return log_has (needle);
}

/* Aborta sem clicar se o binário resolvido não vier do diretório fake. */
static void
require_fake_program (const gchar *dir,
                      const gchar *name)
{
  gchar *found = g_find_program_in_path (name);
  gchar *expected = g_build_filename (dir, name, NULL);

  if (found == NULL)
    g_error ("%s not found; refusing to click with PATH=%s", name, dir);
  if (!g_str_equal (found, expected))
    g_error ("%s resolved to %s, expected %s; refusing to click",
             name, found, expected);
  g_free (found);
  g_free (expected);
}

/* Garante que um executável perigoso não está disponível no PATH atual. */
static void
require_program_absent (const gchar *name)
{
  gchar *found = g_find_program_in_path (name);

  if (found != NULL)
    g_error ("%s unexpectedly resolves to %s; refusing to click", name, found);
  g_free (found);
}

static void
fixture_setup (Fixture *f)
{
  f->qs = g_new0 (QuickSettings, 1);
  qs_ref (f->qs);
  f->popup = gtk_offscreen_window_new ();
  f->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (f->stack),
                                 GTK_STACK_TRANSITION_TYPE_NONE);
  f->main_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (f->stack), f->main_page, "main");
  f->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->qs->popup = f->popup;
  f->qs->stack = f->stack;
  session_build_section (f->box, f->stack, f->qs);
  gtk_container_add (GTK_CONTAINER (f->popup), f->stack);
  /* Mostra só o stack: a janela offscreen nunca é realizada/mapeada, então
   * nenhum ícone do tema é carregado (e o teste pode rodar sem tema). */
  gtk_widget_show_all (f->stack);
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "main");
}

static void
fixture_teardown (Fixture *f)
{
  gtk_widget_destroy (f->box);
  gtk_widget_destroy (f->popup);
  qs_unref (f->qs);
}

static gboolean
widget_has_class (GtkWidget   *widget,
                  const gchar *name)
{
  return gtk_style_context_has_class (gtk_widget_get_style_context (widget),
                                      name);
}

static GtkWidget *
find_by_class (GtkWidget   *root,
               const gchar *name)
{
  if (!GTK_IS_WIDGET (root))
    return NULL;
  if (widget_has_class (root, name))
    return root;
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_by_class (GTK_WIDGET (l->data), name);
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static GtkWidget *
find_button_by_action (GtkWidget   *root,
                       const gchar *action)
{
  if (!GTK_IS_WIDGET (root))
    return NULL;
  if (GTK_IS_BUTTON (root))
    {
      const gchar *value = g_object_get_data (G_OBJECT (root), "qs-action");

      if (g_strcmp0 (value, action) == 0)
        return root;
    }
  if (GTK_IS_CONTAINER (root))
    {
      GList     *kids = gtk_container_get_children (GTK_CONTAINER (root));
      GList     *l;
      GtkWidget *hit = NULL;

      for (l = kids; l != NULL && hit == NULL; l = l->next)
        hit = find_button_by_action (GTK_WIDGET (l->data), action);
      g_list_free (kids);
      return hit;
    }
  return NULL;
}

static void
click_action (GtkWidget   *stack,
              const gchar *action)
{
  GtkWidget *button = find_button_by_action (stack, action);

  g_assert_nonnull (button);
  g_signal_emit_by_name (button, "clicked");
}

/* Caminho principal: xfce4-session-logout falso no PATH. */
static void
test_xfce_actions (void)
{
  static const gchar *actions[] = {
    "halt", "reboot", "hibernate", "suspend", "logout"
  };
  static const gchar *args[] = {
    "--halt", "--reboot", "--hibernate", "--suspend", "--logout"
  };
  Fixture      f;
  gchar       *text;
  guint        i;

  if (skip_no_display ())
    return;

  g_remove (log_path);
  g_setenv ("PATH", fakebin, TRUE);
  require_program_absent ("systemctl");
  require_fake_program (fakebin, "xfce4-session-logout");

  fixture_setup (&f);
  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      gchar *expected;

      require_fake_program (fakebin, "xfce4-session-logout");
      click_action (f.stack, actions[i]);
      expected = g_strdup_printf ("xfce4-session-logout %s", args[i]);
      g_assert_true (wait_log (expected, 2000));
      g_free (expected);
    }

  text = log_read ();
  g_assert_nonnull (text);
  g_assert_null (strstr (text, "systemctl"));
  g_assert_null (strstr (text, "loginctl"));
  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      gchar *expected = g_strdup_printf ("xfce4-session-logout %s", args[i]);

      g_assert_cmpuint (count_occurrences (text, expected), ==, 1);
      g_free (expected);
    }
  g_free (text);

  fixture_teardown (&f);
}

/* Fallback: sem xfce4-session-logout, só systemctl/loginctl falsos. */
static void
test_fallback_actions (void)
{
  static const gchar *actions[] = {
    "halt", "reboot", "hibernate", "suspend"
  };
  static const gchar *args[] = {
    "poweroff", "reboot", "hibernate", "suspend"
  };
  Fixture      f;
  const gchar *user;
  gchar       *quoted_user;
  gchar       *logout_cmd;
  gchar       *text;
  guint        i;

  if (skip_no_display ())
    return;

  g_remove (log_path);
  g_setenv ("PATH", fakebin_session, TRUE);
  require_program_absent ("xfce4-session-logout");
  require_fake_program (fakebin_session, "systemctl");
  require_fake_program (fakebin_session, "loginctl");

  fixture_setup (&f);
  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      gchar *expected;

      require_fake_program (fakebin_session, "systemctl");
      click_action (f.stack, actions[i]);
      expected = g_strdup_printf ("systemctl %s", args[i]);
      g_assert_true (wait_log (expected, 2000));
      g_free (expected);
    }

  require_fake_program (fakebin_session, "loginctl");
  click_action (f.stack, "logout");
  /* qs-session monta "loginctl terminate-user <quoted>"; o fake registra
   * o argv com as aspas já removidas pelo shell, então o log traz o
   * usuário cru. */
  user = g_get_user_name ();
  quoted_user = g_shell_quote (user);
  g_assert_true (g_str_has_prefix (quoted_user, "'"));
  g_assert_true (g_str_has_suffix (quoted_user, "'"));
  logout_cmd = g_strdup_printf ("loginctl terminate-user %s", user);
  g_assert_true (wait_log (logout_cmd, 2000));

  text = log_read ();
  g_assert_nonnull (text);
  g_assert_null (strstr (text, "xfce4-session-logout"));
  g_assert_cmpuint (count_occurrences (text, logout_cmd), ==, 1);
  for (i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      gchar *expected = g_strdup_printf ("systemctl %s", args[i]);

      g_assert_cmpuint (count_occurrences (text, expected), ==, 1);
      g_free (expected);
    }
  g_free (text);

  g_free (logout_cmd);
  g_free (quoted_user);
  fixture_teardown (&f);
}

/* qs-action desconhecida não executa nada. */
static void
test_unknown_action (void)
{
  Fixture *f;
  gchar   *before;
  gchar   *after;

  if (skip_no_display ())
    return;

  g_setenv ("PATH", fakebin_session, TRUE);
  require_fake_program (fakebin_session, "systemctl");
  require_fake_program (fakebin_session, "loginctl");
  g_file_set_contents (log_path, "", 0, NULL);

  f = g_new0 (Fixture, 1);
  fixture_setup (f);
  {
    GtkWidget *button = find_button_by_action (f->stack, "logout");

    g_assert_nonnull (button);
    g_object_set_data (G_OBJECT (button), "qs-action", (gpointer) "unknown");
    before = log_read ();
    g_assert_nonnull (before);
    g_signal_emit_by_name (button, "clicked");
    run_loop_ms (400);
    after = log_read ();
    g_assert_nonnull (after);
    g_assert_cmpstr (after, ==, before);
    g_free (after);
    g_free (before);
  }
  fixture_teardown (f);
  g_free (f);
}

/* Tile Power abre a página "power"; voltar retorna a "main". */
static void
test_power_navigation (void)
{
  Fixture   f;
  GtkWidget *tile;
  GtkWidget *page;
  GtkWidget *back;

  if (skip_no_display ())
    return;

  g_setenv ("PATH", fakebin_session, TRUE);
  require_fake_program (fakebin_session, "systemctl");
  require_fake_program (fakebin_session, "loginctl");
  fixture_setup (&f);
  tile = find_by_class (f.box, "qs-tile");
  g_assert_nonnull (tile);
  g_signal_emit_by_name (tile, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f.stack)),
                   ==, "power");

  page = gtk_stack_get_child_by_name (GTK_STACK (f.stack), "power");
  g_assert_nonnull (page);
  back = find_by_class (page, "qs-back");
  g_assert_nonnull (back);
  g_signal_emit_by_name (back, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f.stack)),
                   ==, "main");

  fixture_teardown (&f);
}

int
main (int argc, char **argv)
{
  gchar *cwd;
  gchar *old_path;
  gint   result;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-session-actions-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  cwd = g_get_current_dir ();
  fakebin = g_build_filename (cwd, "tests", "fakebin", NULL);
  fakebin_session = g_build_filename (cwd, "tests", "fakebin-session", NULL);
  g_assert_true (g_file_test (fakebin, G_FILE_TEST_IS_DIR));
  g_assert_true (g_file_test (fakebin_session, G_FILE_TEST_IS_DIR));
  g_free (cwd);

  log_path = g_build_filename (tmp_root, "fake-argv.log", NULL);
  g_setenv ("QS_FAKE_LOG", log_path, TRUE);
  old_path = g_strdup (g_getenv ("PATH"));

  g_test_add_func ("/session/xfce_actions", test_xfce_actions);
  g_test_add_func ("/session/fallback_actions", test_fallback_actions);
  g_test_add_func ("/session/unknown_action", test_unknown_action);
  g_test_add_func ("/session/power_navigation", test_power_navigation);

  result = g_test_run ();

  if (old_path != NULL)
    g_setenv ("PATH", old_path, TRUE);
  else
    g_unsetenv ("PATH");
  g_free (old_path);
  g_free (log_path);
  g_free (fakebin);
  g_free (fakebin_session);
  g_free (tmp_root);
  return result;
}
