/* qs-notify-store.c — notification history store (sqlite) movido de qs-notify.c.
 * Movimento puro; poll/rows/build permanecem em qs-notify.c. */

#include "qs-notify-store.h"

#include <sqlite3.h>
#include <sys/stat.h>

/* COUNT + MAX(timestamp) em 1 query (tile rápido, sem parse de 30 rows). */
gboolean
notify_db_signature (gint   *count_out,
                     gint64 *max_out)
{
  gchar       *path = notify_db_path ();
  sqlite3     *db = NULL;
  sqlite3_stmt *stmt = NULL;
  gboolean     ok = FALSE;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_free (path);
      return FALSE;
    }
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
      g_free (path);
      if (db != NULL)
        sqlite3_close (db);
      return FALSE;
    }
  sqlite3_busy_timeout (db, 1000);
  g_free (path);
  if (sqlite3_prepare_v2 (db,
                          "SELECT COUNT(*), MAX(timestamp) FROM notifications;",
                          -1, &stmt, NULL) == SQLITE_OK
      && sqlite3_step (stmt) == SQLITE_ROW)
    {
      if (count_out != NULL)
        *count_out = sqlite3_column_int (stmt, 0);
      if (max_out != NULL)
        max_out[0] = sqlite3_column_type (stmt, 1) == SQLITE_NULL
                     ? 0 : sqlite3_column_int64 (stmt, 1);
      ok = TRUE;
    }
  if (stmt != NULL)
    sqlite3_finalize (stmt);
  if (db != NULL)
    sqlite3_close (db);
  return ok;
}

void
notice_free (gpointer data)
{
  Notice *n = data;

  g_free (n->app);
  g_free (n->summary);
  g_free (n->body);
  g_free (n);
}

/* Path to the xfce4-notifyd history database (may not exist).
 * Uses XDG cache dir (usually ~/.cache), not hardcoded HOME/.cache. */
gchar *
notify_db_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "xfce4",
                           "notifyd", "log.sqlite", NULL);
}

/* Flattens newlines: the list splits rows on '\n', so embedded
 * newlines would corrupt parsing. */
gchar *
flat_text (const unsigned char *src)
{
  gchar *s;

  if (src == NULL)
    return g_strdup ("");
  s = g_strdup ((const gchar *) src);
  g_strdelimit (s, "\r\n\x1f", ' ');
  return s;
}

/* Newest first, or NULL when the log is missing/unreadable.
 * Reads via libsqlite3 directly: no sqlite3 CLI dependency and no
 * separator-quoting fragility. */
GList *
list_notices (void)
{
  gchar     *path = notify_db_path ();
  sqlite3   *db = NULL;
  sqlite3_stmt *stmt = NULL;
  GList     *notices = NULL;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_free (path);
      return NULL;
    }
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
      g_free (path);
      if (db != NULL)
        sqlite3_close (db);
      return NULL;
    }
  sqlite3_busy_timeout (db, 1000);
  g_free (path);
  if (sqlite3_prepare_v2 (db,
                           "SELECT app_name, summary, body, timestamp "
                          "FROM notifications "
                          "ORDER BY timestamp DESC LIMIT ?;",
                          -1, &stmt, NULL) != SQLITE_OK)
    {
      sqlite3_close (db);
      return NULL;
    }
  sqlite3_bind_int (stmt, 1, NOTIFY_MAX_ROWS);
  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Notice *n = g_new0 (Notice, 1);

      n->app = flat_text (sqlite3_column_text (stmt, 0));
      n->summary = flat_text (sqlite3_column_text (stmt, 1));
      n->body = flat_text (sqlite3_column_text (stmt, 2));
      /* xfce4-notifyd stores microseconds since epoch (see
       * xfce-notify-log.c: unix * 1e6 + usec), not seconds. */
      n->timestamp = sqlite3_column_int64 (stmt, 3);
      notices = g_list_append (notices, n);
    }
  sqlite3_finalize (stmt);
  sqlite3_close (db);
  return notices;
}

/* "14:32" for today, "09/10 14:32" otherwise.
 * xfce4-notifyd stores microseconds; old DBs may store seconds.
 * Heuristic: values below 4102444800 (2100-01-01 em segundos)
 * são tratados como segundos. */
gchar *
notice_time (gint64 timestamp_usec)
{
  gint64 timestamp = (timestamp_usec < 4102444800LL)
                     ? timestamp_usec : timestamp_usec / 1000000;
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (timestamp);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  gint year, month, day;

  if (dt == NULL || now == NULL)
    return g_strdup ("");
  g_date_time_get_ymd (now, &year, &month, &day);
  if (g_date_time_get_year (dt) == year
      && g_date_time_get_month (dt) == month
      && g_date_time_get_day_of_month (dt) == day)
    return g_date_time_format (dt, "%H:%M");
  return g_date_time_format (dt, "%d/%m %H:%M");
}
