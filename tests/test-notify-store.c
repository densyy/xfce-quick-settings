/* Testes do qs-notify-store: flat_text, notice_time e backend sqlite.
 *
 * Isolamento: XDG_CACHE_HOME temporário (setado 1x no main); o banco
 * de teste é criado sob <tmp>/xfce4/notifyd/log.sqlite.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "qs-notify-store.h"

static gchar *tmp_root = NULL;

static gchar *
db_path (void)
{
  return g_build_filename (tmp_root, "xfce4", "notifyd", "log.sqlite", NULL);
}

/* Cria um banco com nrows linhas; timestamps = base+i (microssegundos). */
static void
make_db (gint nrows, gint64 base_ts)
{
  gchar   *path = db_path ();
  gchar   *dir = g_path_get_dirname (path);
  sqlite3 *db = NULL;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_remove (path);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
                                 "CREATE TABLE notifications("
                                 "app_name TEXT, summary TEXT, body TEXT, "
                                 "timestamp INTEGER);",
                                 NULL, NULL, NULL),
                   ==, SQLITE_OK);
  for (gint i = 0; i < nrows; i++)
    {
      gchar *sql = g_strdup_printf ("INSERT INTO notifications VALUES("
                                    "'App%d', 'Sum%d', 'Body%d', %"
                                    G_GINT64_FORMAT ");",
                                    i, i, i, base_ts + (gint64) i);

      g_assert_cmpint (sqlite3_exec (db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
      g_free (sql);
    }
  g_assert_cmpint (sqlite3_close (db), ==, SQLITE_OK);
  g_free (dir);
  g_free (path);
}

static void
test_flat_null (void)
{
  gchar *flat = flat_text (NULL);
  g_assert_cmpstr (flat, ==, "");
  g_free (flat);
}

static void
test_flat_basic (void)
{
  gchar *flat = flat_text ((const guchar *) "a\nb\rc\x1f"
                                             "d");

  g_assert_cmpstr (flat, ==, "a b c d");
  g_free (flat);
  flat = flat_text ((const guchar *) "plain");
  g_assert_cmpstr (flat, ==, "plain");
  g_free (flat);
}

/* Timestamp antigo em segundos usa "%d/%m %H:%M" (11 chars). */
static void
test_time_old (void)
{
  gchar *text = notice_time (946684800); /* 2000-01-01 00:00:00 UTC */

  g_assert_nonnull (text);
  g_assert_cmpuint (strlen (text), ==, 11);
  g_free (text);
}

/* Timestamp de agora em microssegundos usa "%H:%M" (5 chars). */
static void
test_time_today (void)
{
  gint64 now_us = g_get_real_time ();
  gchar *text = notice_time (now_us);

  g_assert_nonnull (text);
  g_assert_cmpuint (strlen (text), ==, 5);
  g_assert_cmpuint ((guint) text[2], ==, (guint) ':');
  g_free (text);
}

/* 35 linhas → retorna 30, em ordem DESC de timestamp. */
static void
test_sqlite_roundtrip (void)
{
  gint64  base = g_get_real_time () - 3600 * 1000000;
  GList  *notices;
  Notice *first;
  Notice *last;

  make_db (35, base);
  notices = list_notices ();
  g_assert_nonnull (notices);
  g_assert_cmpuint (g_list_length (notices), ==, 30);
  first = (Notice *) notices->data;
  last = (Notice *) g_list_last (notices)->data;
  g_assert_cmpint (first->timestamp, ==, base + 34);
  g_assert_cmpint (last->timestamp, ==, base + 5);
  g_assert_cmpstr (first->app, ==, "App34");
  g_list_free_full (notices, notice_free);
}

/* signature = COUNT + MAX(timestamp). */
static void
test_sqlite_signature (void)
{
  gint64 base = g_get_real_time () - 3600 * 1000000;
  gint   count = 0;
  gint64 max_ts = 0;

  make_db (35, base);
  g_assert_true (notify_db_signature (&count, &max_ts));
  g_assert_cmpint (count, ==, 35);
  g_assert_cmpint (max_ts, ==, base + 34);
}

/* Banco sem a tabela: FALSE/NULL sem crash. */
static void
test_sqlite_no_table (void)
{
  gchar   *path = db_path ();
  gchar   *dir = g_path_get_dirname (path);
  sqlite3 *db = NULL;
  gint     count = -1;
  gint64   max_ts = -1;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_remove (path);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_close (db), ==, SQLITE_OK);
  g_assert_null (list_notices ());
  g_assert_false (notify_db_signature (&count, &max_ts));
  g_free (dir);
  g_free (path);
}

/* Banco ausente: NULL/FALSE sem crash. */
static void
test_sqlite_missing (void)
{
  gchar  *path = db_path ();
  gint    count = -1;
  gint64  max_ts = -1;

  g_remove (path);
  g_assert_null (list_notices ());
  g_assert_false (notify_db_signature (&count, &max_ts));
  g_free (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-notify-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);

  g_test_add_func ("/notify-store/flat_null", test_flat_null);
  g_test_add_func ("/notify-store/flat_basic", test_flat_basic);
  g_test_add_func ("/notify-store/time_old", test_time_old);
  g_test_add_func ("/notify-store/time_today", test_time_today);
  g_test_add_func ("/notify-store/sqlite_roundtrip", test_sqlite_roundtrip);
  g_test_add_func ("/notify-store/sqlite_signature", test_sqlite_signature);
  g_test_add_func ("/notify-store/sqlite_no_table", test_sqlite_no_table);
  g_test_add_func ("/notify-store/sqlite_missing", test_sqlite_missing);
  return g_test_run ();
}
