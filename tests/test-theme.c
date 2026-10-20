/* test-theme.c — carregamento do stylesheet compartilhado.
 *
 * Precisa de display (o provider é instalado no screen default). Sem
 * display o caso dá skip. Chamadas repetidas precisam ser no-op.
 */
#include <glib.h>
#include <gtk/gtk.h>

#include "qs-theme.h"

static gboolean have_display = FALSE;

static void
test_theme_load_idempotent (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return;
    }

  qs_theme_load ();
  qs_theme_load ();
  qs_theme_load ();

  g_assert_nonnull (gdk_screen_get_default ());
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/theme/load_idempotent", test_theme_load_idempotent);

  return g_test_run ();
}
