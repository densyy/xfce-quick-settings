#include "qs-util.h"

#include <gio/gio.h>

gboolean
command_exists (const gchar *name)
{
  gchar *path = g_find_program_in_path (name);

  if (path != NULL)
    {
      g_free (path);
      return TRUE;
    }
  return FALSE;
}

/* Runs cmdline synchronously, returns stdout (caller frees) or NULL. */
gchar *
run_command (const gchar *cmdline)
{
  gchar  *output = NULL;
  GError *error = NULL;

  if (!g_spawn_command_line_sync (cmdline, &output, NULL, NULL, &error))
    {
      g_warning ("run_command failed: %s: %s", cmdline, error->message);
      g_clear_error (&error);
      return NULL;
    }
  return output;
}

void
spawn_command (const gchar *cmdline)
{
  GError *error = NULL;

  if (!g_spawn_command_line_async (cmdline, &error))
    {
      g_warning ("spawn_command failed: %s: %s", cmdline, error->message);
      g_clear_error (&error);
    }
}

/* Async command: fully async GSubprocess in the main context, with a
 * watchdog timeout so a stuck binary (pactl/nmcli) can never hang us. */

#define QS_ASYNC_TIMEOUT_SECONDS 5

typedef struct
{
  gchar        *cmdline;
  QsCmdDone     done;
  gpointer      user_data;
  gchar        *output;
  gchar        *errout;
  gboolean      ok;
  gboolean      finished;
  GSubprocess  *proc;
  GCancellable *cancellable;
  guint         timeout_id;
} QsAsync;

static void
qs_async_finish (QsAsync *a)
{
  if (a->finished)
    return;
  a->finished = TRUE;
  if (a->timeout_id != 0)
    {
      g_source_remove (a->timeout_id);
      a->timeout_id = 0;
    }
  if (a->done != NULL)
    a->done (a->output, a->errout, a->ok, a->user_data);
  g_clear_object (&a->proc);
  g_clear_object (&a->cancellable);
  g_free (a->cmdline);
  g_free (a->output);
  g_free (a->errout);
  g_free (a);
}

static void
qs_async_comm_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  QsAsync *a = user_data;
  GError  *error = NULL;
  GBytes  *out = NULL;
  GBytes  *err = NULL;

  if (g_subprocess_communicate_finish (G_SUBPROCESS (source), result,
                                       &out, &err, &error))
    {
      if (out != NULL)
        {
          gsize len;
          const gchar *p = g_bytes_get_data (out, &len);

          a->output = (p != NULL && len > 0) ? g_strndup (p, len)
                                             : g_strdup ("");
        }
      if (err != NULL)
        {
          gsize len;
          const gchar *p = g_bytes_get_data (err, &len);

          a->errout = (p != NULL && len > 0) ? g_strndup (p, len)
                                             : g_strdup ("");
        }
      a->ok = TRUE;
    }
  else
    {
      /* timeout/cancelamento ou falha de I/O: não pode travar.
       * force_exit é seguro mesmo se o processo já tiver saído e evita a
       * assertion de g_subprocess_get_if_exited em processo ainda vivo. */
      if (a->proc != NULL)
        g_subprocess_force_exit (a->proc);
      a->ok = FALSE;
      g_clear_error (&error);
    }
  if (out != NULL)
    g_bytes_unref (out);
  if (err != NULL)
    g_bytes_unref (err);
  qs_async_finish (a);
}

static gboolean
qs_async_timeout (gpointer user_data)
{
  QsAsync *a = user_data;

  a->timeout_id = 0;
  if (!a->finished && a->cancellable != NULL)
    g_cancellable_cancel (a->cancellable);
  return G_SOURCE_REMOVE;
}

static gboolean
qs_async_launch (gpointer user_data)
{
  QsAsync *a = user_data;
  GSubprocessLauncher *launcher;
  GError *error = NULL;
  gchar **argv = NULL;

  if (!g_shell_parse_argv (a->cmdline, NULL, &argv, &error))
    {
      g_clear_error (&error);
      a->ok = FALSE;
      qs_async_finish (a);
      return G_SOURCE_REMOVE;
    }

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                        | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  a->proc = g_subprocess_launcher_spawnv (launcher,
                                          (const gchar * const *) argv,
                                          &error);
  g_object_unref (launcher);
  g_strfreev (argv);

  if (a->proc == NULL)
    {
      g_clear_error (&error);
      a->ok = FALSE;
      qs_async_finish (a);
      return G_SOURCE_REMOVE;
    }

  a->cancellable = g_cancellable_new ();
  a->timeout_id = g_timeout_add_seconds (QS_ASYNC_TIMEOUT_SECONDS,
                                         qs_async_timeout, a);
  /* GLib 2.88: g_subprocess_communicate_async ganhou o parâmetro
   * stdin_buf; NULL = sem stdin, equivalente à assinatura antiga de 4 args. */
  g_subprocess_communicate_async (a->proc, NULL, a->cancellable,
                                  qs_async_comm_done, a);
  return G_SOURCE_REMOVE;
}

void
run_command_async (const gchar *cmdline,
                   QsCmdDone    done,
                   gpointer     user_data)
{
  QsAsync *a;

  if (cmdline == NULL || done == NULL)
    return;

  a = g_new0 (QsAsync, 1);
  a->cmdline = g_strdup (cmdline);
  a->done = done;
  a->user_data = user_data;
  /* Agenda o launch no main context global como fonte (g_timeout_add é
   * thread-safe). Garante que spawn, timeout e done rodem SEMPRE dentro de
   * uma iteração do loop — nunca de forma síncrona ao chamador. Isso evita
   * que done()/g_main_loop_quit() dispare antes de g_main_loop_run() quando
   * a falha é imediata (ex.: binário inexistente). Sem threads. */
  g_timeout_add (0, qs_async_launch, a);
}

/* Split an nmcli terse line on unescaped ':' (handles \: and \\). */
gchar **
split_terse_line (const gchar *line,
                  gint         expected_fields)
{
  GPtrArray   *fields = g_ptr_array_new ();
  GString     *cur = g_string_new (NULL);
  const gchar *p = line;

  while (*p != '\0')
    {
      if (*p == '\\' && *(p + 1) != '\0')
        {
          g_string_append_c (cur, *(p + 1));
          p += 2;
        }
      else if (*p == ':' && fields->len < (guint) (expected_fields - 1))
        {
          g_ptr_array_add (fields, g_strdup (cur->str));
          g_string_truncate (cur, 0);
          p++;
        }
      else
        {
          g_string_append_c (cur, *p);
          p++;
        }
    }
  g_ptr_array_add (fields, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  while (fields->len < (guint) expected_fields)
    g_ptr_array_add (fields, g_strdup (""));
  g_ptr_array_add (fields, NULL);
  return (gchar **) g_ptr_array_free (fields, FALSE);
}

/* Parse a decimal integer with full validation: rejects NULL/empty, signs,
 * trailing garbage and overflow past G_MAXINT. Returns FALSE on any invalid
 * input, TRUE (writing *out) otherwise. */
gboolean
qs_parse_int_checked (const gchar *s,
                      gint        *out)
{
  const gchar *p;
  gchar *end = NULL;
  guint64 v;

  if (s == NULL || out == NULL)
    return FALSE;
  p = s;
  while (g_ascii_isspace (*p))
    p++;
  if (*p == '\0')
    return FALSE;
  if (*p == '-')
    return FALSE;
  if (*p == '+')
    {
      p++;
      if (*p == '\0')
        return FALSE;
    }
  errno = 0;
  v = g_ascii_strtoull (p, &end, 10);
  if (errno != 0 || end == p)
    return FALSE;
  while (*end != '\0' && g_ascii_isspace (*end))
    end++;
  if (*end != '\0')
    return FALSE;
  if (v > (guint64) G_MAXINT)
    return FALSE;
  *out = (gint) v;
  return TRUE;
}

/* First "NN%" found in text, or -1. */
gint
parse_first_percent (const gchar *text)
{
  const gchar *p;
  const gchar *start;
  gchar       *num;
  gint         value;

  if (text == NULL)
    return -1;
  p = strchr (text, '%');
  if (p == NULL)
    return -1;
  start = p;
  while (start > text && g_ascii_isdigit (*(start - 1)))
    start--;
  if (start == p)
    return -1;
  num = g_strndup (start, (gsize) (p - start));
  if (!qs_parse_int_checked (num, &value))
    value = -1;
  g_free (num);
  return value;
}
