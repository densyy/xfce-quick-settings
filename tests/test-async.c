/* Testes do grupo 2: run_command_async (GSubprocess + watchdog, com main loop). */
#include <glib.h>

#include "qs-util.h"

typedef struct
{
  gchar    *output;
  gchar    *errout;
  gboolean  ok;
  gint      calls;
} AsyncCap;

static GMainLoop *async_loop = NULL;
static gboolean   async_timed_out = FALSE;

static gboolean
async_quit (gpointer data)
{
  async_timed_out = TRUE;
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static void
async_on_done (const gchar *output,
               const gchar *errout,
               gboolean     ok,
               gpointer     user_data)
{
  AsyncCap *c = user_data;

  g_free (c->output);
  g_free (c->errout);
  c->output = g_strdup (output);
  c->errout = g_strdup (errout);
  c->ok = ok;
  c->calls++;
  g_main_loop_quit (async_loop);
}

/* Roda cmd e espera done (rede de segurança de 20s contra hang). */
static void
async_run (const gchar *cmdline, AsyncCap *c)
{
  guint safety;

  async_loop = g_main_loop_new (NULL, FALSE);
  async_timed_out = FALSE;
  safety = g_timeout_add_seconds (20, async_quit, async_loop);
  run_command_async (cmdline, async_on_done, c);
  g_main_loop_run (async_loop);
  g_source_remove (safety);
  g_assert_false (async_timed_out);
  g_main_loop_unref (async_loop);
  async_loop = NULL;
}

static void
test_async_echo (void)
{
  AsyncCap c = { NULL, NULL, FALSE, 0 };

  async_run ("printf 'hello async'", &c);
  g_assert_cmpint (c.calls, ==, 1);
  g_assert_true (c.ok);
  g_assert_cmpstr (c.output, ==, "hello async");
  g_free (c.output);
  g_free (c.errout);
}

static void
test_async_missing (void)
{
  AsyncCap c = { NULL, NULL, TRUE, 0 };

  /* binário inexistente: done exatamente 1x com ok=FALSE (regressão do
   * bug do launch inline antes do main_loop_run). */
  async_run ("definitely-not-a-real-binary-xyz --foo", &c);
  g_assert_cmpint (c.calls, ==, 1);
  g_assert_false (c.ok);
  g_free (c.output);
  g_free (c.errout);
}

static void
test_async_watchdog (void)
{
  AsyncCap c = { NULL, NULL, TRUE, 0 };
  GTimer  *t = g_timer_new ();
  gdouble  elapsed;

  /* sleep 30 deve ser cancelado pelo watchdog de 5s (QS_ASYNC_TIMEOUT_SECONDS). */
  g_timer_start (t);
  async_run ("sleep 30", &c);
  elapsed = g_timer_elapsed (t, NULL);
  g_timer_destroy (t);
  g_assert_cmpint (c.calls, ==, 1);
  g_assert_false (c.ok);
  g_assert_true (elapsed >= 4.0 && elapsed < 19.0);
  g_free (c.output);
  g_free (c.errout);
}

static void
test_async_null (void)
{
  AsyncCap c = { NULL, NULL, FALSE, 0 };

  /* NULL cmdline/done = no-op imediato, sem crash e sem callback. */
  run_command_async (NULL, async_on_done, &c);
  run_command_async ("printf hi", NULL, NULL);
  g_assert_cmpint (c.calls, ==, 0);
}

static void
test_async_quoting (void)
{
  AsyncCap c = { NULL, NULL, FALSE, 0 };

  async_run ("printf '%s' 'a b c'", &c);
  g_assert_cmpint (c.calls, ==, 1);
  g_assert_true (c.ok);
  g_assert_cmpstr (c.output, ==, "a b c");
  g_free (c.output);
  g_free (c.errout);
}

static void
test_async_exit_ignored (void)
{
  AsyncCap c = { NULL, NULL, FALSE, 0 };

  /* exit code != 0 NÃO é erro: ok=TRUE se spawn+communicate funcionaram. */
  async_run ("ls /definitely-not-here-xyz", &c);
  g_assert_cmpint (c.calls, ==, 1);
  g_assert_true (c.ok);
  g_assert_nonnull (c.errout);
  g_assert_true (*c.errout != '\0');
  g_free (c.output);
  g_free (c.errout);
}

gint
main (gint argc, gchar **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/async/echo", test_async_echo);
  g_test_add_func ("/async/missing", test_async_missing);
  g_test_add_func ("/async/watchdog", test_async_watchdog);
  g_test_add_func ("/async/null", test_async_null);
  g_test_add_func ("/async/quoting", test_async_quoting);
  g_test_add_func ("/async/exit_ignored", test_async_exit_ignored);
  return g_test_run ();
}
