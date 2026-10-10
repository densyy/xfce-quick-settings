#include "qs-util.h"

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
    g_clear_error (&error);
}

/* Async command: runs in a worker thread, done() fires in the main loop. */

typedef struct
{
  gchar    *cmdline;
  QsCmdDone done;
  gpointer  user_data;
  gchar    *output;
  gchar    *errout;
  gboolean  ok;
} QsAsync;

static gboolean
qs_async_idle (gpointer data)
{
  QsAsync *a = data;

  if (a->done != NULL)
    a->done (a->output, a->errout, a->ok, a->user_data);
  g_free (a->cmdline);
  g_free (a->output);
  g_free (a->errout);
  g_free (a);
  return G_SOURCE_REMOVE;
}

static gpointer
qs_async_thread (gpointer data)
{
  QsAsync *a = data;
  GError  *error = NULL;

  a->ok = g_spawn_command_line_sync (a->cmdline, &a->output, &a->errout,
                                     NULL, &error);
  if (error != NULL)
    g_clear_error (&error);
  g_idle_add (qs_async_idle, a);
  return NULL;
}

void
run_command_async (const gchar *cmdline,
                   QsCmdDone    done,
                   gpointer     user_data)
{
  QsAsync *a;
  GThread *thread;

  if (cmdline == NULL || done == NULL)
    return;

  a = g_new0 (QsAsync, 1);
  a->cmdline = g_strdup (cmdline);
  a->done = done;
  a->user_data = user_data;
  thread = g_thread_new ("qs-cmd", qs_async_thread, a);
  if (thread == NULL)
    {
      g_free (a->cmdline);
      g_free (a);
      return;
    }
  g_thread_unref (thread);
}

/* qs-aware variant: the ref keeps qs alive across the worker thread, so
 * done() never runs with a freed QuickSettings. done() is skipped when
 * qs died meanwhile (free-data ran); the ref is always dropped here, so
 * done() must NOT call qs_unref for this helper (unlike the manual
 * qs_ref/qs_unref pattern used with run_command_async). */
typedef struct
{
  gchar         *cmdline;
  QsCmdDone      done;
  QuickSettings *qs;
  gchar         *output;
  gchar         *errout;
  gboolean       ok;
} QsAsyncQs;

static gboolean
qs_async_qs_idle (gpointer data)
{
  QsAsyncQs *a = data;

  if (!a->qs->dead && a->done != NULL)
    a->done (a->output, a->errout, a->ok, a->qs);
  qs_unref (a->qs);
  g_free (a->cmdline);
  g_free (a->output);
  g_free (a->errout);
  g_free (a);
  return G_SOURCE_REMOVE;
}

static gpointer
qs_async_qs_thread (gpointer data)
{
  QsAsyncQs *a = data;
  GError    *error = NULL;

  a->ok = g_spawn_command_line_sync (a->cmdline, &a->output, &a->errout,
                                     NULL, &error);
  if (error != NULL)
    g_clear_error (&error);
  g_idle_add (qs_async_qs_idle, a);
  return NULL;
}

void
run_command_async_qs (const gchar   *cmdline,
                      QsCmdDone      done,
                      QuickSettings *qs)
{
  QsAsyncQs *a;
  GThread   *thread;

  if (cmdline == NULL || done == NULL || qs == NULL)
    return;

  qs_ref (qs);
  a = g_new0 (QsAsyncQs, 1);
  a->cmdline = g_strdup (cmdline);
  a->done = done;
  a->qs = qs;
  thread = g_thread_new ("qs-cmd", qs_async_qs_thread, a);
  if (thread == NULL)
    {
      qs_unref (qs);
      g_free (a->cmdline);
      g_free (a);
      return;
    }
  g_thread_unref (thread);
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
  value = atoi (num);
  g_free (num);
  return value;
}
