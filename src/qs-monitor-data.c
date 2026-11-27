/* qs-monitor-data.c — snapshot xrandr DATA (movido de qs-monitor.c).
 * Movimento puro; UI permanece em qs-monitor.c. Nenhum GTK aqui. */

#include "qs-monitor-data.h"

#include <string.h>

/* Valid output name ([A-Za-z0-9_.-], xrandr never emits anything else).
 * Rejected names are skipped at parse time and never applied. */
gboolean
valid_monitor_shared (const gchar *s)
{
  const gchar *p;

  if (s == NULL || *s == '\0' || strlen (s) > 64)
    return FALSE;
  for (p = s; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_' && *p != '.')
      return FALSE;
  return TRUE;
}

gboolean
valid_mode (const gchar *s)
{
  const gchar *x;
  const gchar *p;

  if (s == NULL || !g_ascii_isdigit (*s))
    return FALSE;
  x = strchr (s, 'x');
  if (x == NULL || x == s || x[1] == '\0')
    return FALSE;
  for (p = s; p < x; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  for (p = x + 1; *p != '\0'; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return TRUE;
}

gboolean
valid_scale (const gchar *s)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    if (g_strcmp0 (s, SCALE_FACTORS[i]) == 0)
      return TRUE;
  return FALSE;
}

void
qs_mon_info_free (gpointer data)
{
  QsMonInfo *m = data;

  if (m == NULL)
    return;
  g_free (m->name);
  g_list_free_full (m->modes, g_free);
  g_free (m->current);
  g_free (m);
}

void
qs_display_snap_free (QsDisplaySnap *snap)
{
  if (snap == NULL)
    return;
  g_list_free_full (snap->mons, qs_mon_info_free);
  g_free (snap);
}

/* Geometry token ("1920x1080+0+0") means the output drives pixels. */
gboolean
header_has_geometry (const gchar *header)
{
  gchar    **tok;
  gint       t;
  gboolean   active = FALSE;

  if (header == NULL)
    return FALSE;
  tok = g_strsplit_set (header, " \t", -1);
  for (t = 0; tok[t] != NULL; t++)
    {
      if (g_ascii_isdigit (tok[t][0])
          && strchr (tok[t], 'x') != NULL
          && strchr (tok[t], '+') != NULL)
        {
          active = TRUE;
          break;
        }
    }
  g_strfreev (tok);
  return active;
}

static void
snap_add_mode (QsMonInfo *cur,
               const gchar *line)
{
  if (line == NULL || cur == NULL)
    return;
  gchar *s = g_strdup (line);
  gchar *end;

  g_strstrip (s);
  end = strpbrk (s, " \t");
  if (end != NULL)
    *end = '\0';
  if (valid_mode (s))
    {
      cur->modes = g_list_append (cur->modes, g_strdup (s));
      if (cur->current == NULL && strchr (line, '*') != NULL)
        cur->current = g_strdup (s);
    }
  g_free (s);
}

static QsMonInfo *
snap_add_header (QsDisplaySnap *snap,
                 const gchar   *line)
{
  gchar *s = g_strdup (line);
  gchar **tok;
  gint t;
  gboolean connected = FALSE;
  QsMonInfo *m = NULL;

  g_strstrip (s);
  tok = g_strsplit_set (s, " \t", -1);
  for (t = 1; tok[t] != NULL; t++)
    if (g_strcmp0 (tok[t], "connected") == 0)
      connected = TRUE;
  if (connected && tok[0] != NULL && valid_monitor_shared (tok[0]))
    {
      m = g_new0 (QsMonInfo, 1);
      m->name = g_strdup (tok[0]);
      m->header_active = header_has_geometry (line);
      snap->mons = g_list_append (snap->mons, m);
    }
  g_strfreev (tok);
  g_free (s);
  return m;
}

/* One g_strsplit pass builds monitors + modes + current + counters. */
QsDisplaySnap *
qs_display_snap_new (const gchar *output)
{
  QsDisplaySnap *snap = g_new0 (QsDisplaySnap, 1);
  gchar **lines;
  gint i;
  QsMonInfo *cur = NULL;

  if (output == NULL)
    return snap;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *line = lines[i];

      if (*line == '\0')
        continue;
      if (*line == ' ' || *line == '\t')
        {
          if (cur != NULL)
            snap_add_mode (cur, line);
        }
      else
        cur = snap_add_header (snap, line);
    }
  g_strfreev (lines);

  snap->n_connected = (gint) g_list_length (snap->mons);
  for (GList *l = snap->mons; l != NULL; l = l->next)
    {
      QsMonInfo *m = l->data;

      m->active = m->header_active || (m->current != NULL);
      if (m->active)
        snap->n_active++;
    }
  return snap;
}

QsMonInfo *
qs_snap_find (QsDisplaySnap *snap,
              const gchar   *name)
{
  GList *l;

  if (snap == NULL || !valid_monitor_shared (name))
    return NULL;
  for (l = snap->mons; l != NULL; l = l->next)
    {
      QsMonInfo *m = l->data;

      if (g_strcmp0 (m->name, name) == 0)
        return m;
    }
  return NULL;
}
