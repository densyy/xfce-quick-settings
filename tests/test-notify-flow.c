/* test-notify-flow.c — fluxo da página de notificações (grupo NOTIFY).
 *
 * Dirige a UI real de notify_build_section: linhas expansíveis, contagem
 * do tile, clear, debounce de 500ms, polling de 3s, navegação e teardown.
 * O banco sqlite real fica em XDG_CACHE_HOME/xfce4/notifyd/log.sqlite.
 *
 * Precisa de display para GTK; a janela é GtkOffscreenWindow, portanto
 * nunca é mapeada. Sem display os casos dão skip.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <sqlite3.h>
#include <time.h>
#include <utime.h>

#include "qs-core.h"
#include "qs-cache.h"
#include "qs-notify.h"
#include "qs-notify-store.h"

#define BODY_KEY "qs-notice-body"
#define EXPANDED_KEY "qs-notice-expanded"
#define DEB_KEY "qs-notify-debounce"
#define POLL_KEY "qs-notify-poll"
#define MON_KEY "qs-notify-monitor"

typedef struct
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GtkWidget     *stack;
  GtkWidget     *box;
  GtkWidget     *main_page;
  GtkWidget     *tile;
  GtkWidget     *back;
} Fixture;

static gboolean have_display = FALSE;
static gchar   *tmp_root = NULL;

static guint list_count (GtkWidget *list);

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

static void
wait_label (GtkWidget   *label,
            const gchar *text,
            guint        timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms
         && g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text) != 0)
    {
      run_loop_ms (50);
      waited += 50;
    }
}

static void
wait_list_count (GtkWidget *list,
                 guint      count,
                 guint      timeout_ms)
{
  guint waited = 0;

  while (waited < timeout_ms && list_count (list) != count)
    {
      run_loop_ms (50);
      waited += 50;
    }
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

/* --- sqlite de notificações no caminho usado pelo módulo --- */

static sqlite3 *
db_create (void)
{
  gchar   *path = notify_db_path ();
  gchar   *dir = g_path_get_dirname (path);
  sqlite3 *db = NULL;

  g_mkdir_with_parents (dir, 0700);
  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    g_rmdir (path);
  else
    g_remove (path);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
                                 "CREATE TABLE notifications("
                                 "app_name TEXT, summary TEXT, body TEXT, "
                                 "timestamp INTEGER);",
                                 NULL, NULL, NULL),
                   ==, SQLITE_OK);
  g_free (dir);
  g_free (path);
  return db;
}

static void
db_insert (sqlite3     *db,
           const gchar *app,
           const gchar *summary,
           const gchar *body,
           gint64       timestamp)
{
  sqlite3_stmt *stmt = NULL;

  g_assert_cmpint (sqlite3_prepare_v2 (db,
                                       "INSERT INTO notifications VALUES"
                                       "(?, ?, ?, ?);",
                                       -1, &stmt, NULL), ==, SQLITE_OK);
  sqlite3_bind_text (stmt, 1, app, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, summary, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, body, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, timestamp);
  g_assert_cmpint (sqlite3_step (stmt), ==, SQLITE_DONE);
  sqlite3_finalize (stmt);
}

/* Mtime em segundos: força um valor novo por commit para o fast-path
 * de stat ver a mudança mesmo quando o tamanho do sqlite não muda. */
static void
db_bump_mtime (void)
{
  static gint   seq = 0;
  struct utimbuf times;
  gchar         *path = notify_db_path ();

  times.actime = time (NULL);
  times.modtime = time (NULL) + (++seq);
  g_utime (path, &times);
  g_free (path);
}

static void
db_close (sqlite3 *db)
{
  g_assert_cmpint (sqlite3_close (db), ==, SQLITE_OK);
  db_bump_mtime ();
}

/* --- fixture de UI --- */

static GtkWidget *
first_child (GtkWidget *widget)
{
  GList     *kids = gtk_container_get_children (GTK_CONTAINER (widget));
  GtkWidget *child = kids != NULL ? kids->data : NULL;

  g_list_free (kids);
  return child;
}

static void
fixture_init (Fixture *f)
{
  GtkWidget *page;
  GtkWidget *header;

  g_assert_nonnull (f);
  f->qs = g_new0 (QuickSettings, 1);
  qs_ref (f->qs);
  f->popup = gtk_offscreen_window_new ();
  f->stack = gtk_stack_new ();
  f->main_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  f->qs->popup = f->popup;
  f->qs->stack = f->stack;
  gtk_box_pack_start (GTK_BOX (f->main_page), f->box, FALSE, FALSE, 0);
  gtk_stack_add_named (GTK_STACK (f->stack), f->main_page, "main");
  notify_build_section (f->box, f->stack, f->qs);
  gtk_container_add (GTK_CONTAINER (f->popup), f->stack);
  gtk_widget_show_all (f->stack);
  f->tile = first_child (f->box);
  page = gtk_stack_get_child_by_name (GTK_STACK (f->stack), "notices");
  g_assert_nonnull (page);
  header = first_child (page);
  g_assert_nonnull (header);
  f->back = first_child (header);
  g_assert_nonnull (f->back);
}

static void
fixture_destroy (Fixture *f)
{
  qs_cache_stop ();
  if (f->popup != NULL)
    {
      gtk_widget_destroy (f->popup);
      f->popup = NULL;
      f->qs->popup = NULL;
    }
  qs_unref (f->qs);
  f->qs = NULL;
}

static guint
timer_id (GtkWidget *popup, const gchar *key)
{
  return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (popup), key));
}

static guint
list_count (GtkWidget *list)
{
  GList *kids = gtk_container_get_children (GTK_CONTAINER (list));
  guint  n = g_list_length (kids);

  g_list_free (kids);
  return n;
}

static GtkWidget *
row_at (GtkWidget *list, guint index)
{
  GList     *kids = gtk_container_get_children (GTK_CONTAINER (list));
  GtkWidget *row = g_list_nth_data (kids, index);

  g_list_free (kids);
  return row;
}

static GtkWidget *
row_body (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (row), BODY_KEY);
}

static const gchar *
row_title (GtkWidget *row)
{
  GtkWidget *vbox = first_child (row);
  GtkWidget *header;
  GtkWidget *title;

  if (vbox == NULL)
    return NULL;
  header = first_child (vbox);
  if (header == NULL)
    return NULL;
  title = first_child (header);
  if (title == NULL || !GTK_IS_LABEL (title))
    return NULL;
  return gtk_label_get_text (GTK_LABEL (title));
}

static const gchar *
row_preview (GtkWidget *row)
{
  GtkWidget *body = row_body (row);

  if (body != NULL)
    return gtk_label_get_text (GTK_LABEL (body));
  {
    GtkWidget *vbox = first_child (row);
    GList     *kids;
    GtkWidget *preview = NULL;

    if (vbox == NULL)
      return NULL;
    kids = gtk_container_get_children (GTK_CONTAINER (vbox));
    if (kids != NULL && kids->next != NULL)
      preview = kids->next->data;
    g_list_free (kids);
    if (preview == NULL || !GTK_IS_LABEL (preview))
      return NULL;
    return gtk_label_get_text (GTK_LABEL (preview));
  }
}

static GtkWidget *
find_row (GtkWidget *list, const gchar *preview)
{
  guint n = list_count (list);
  guint i;

  for (i = 0; i < n; i++)
    {
      GtkWidget *row = row_at (list, i);

      if (g_strcmp0 (row_preview (row), preview) == 0)
        return row;
    }
  return NULL;
}

static void
click_row (GtkWidget *row, guint button)
{
  GdkEvent  *event = gdk_event_new (GDK_BUTTON_PRESS);
  GdkWindow *window = gtk_widget_get_window (row);
  gboolean   handled = FALSE;

  event->button.button = button;
  if (window != NULL)
    event->button.window = g_object_ref (window);
  g_signal_emit_by_name (row, "button-press-event", event, &handled);
  g_assert_false (handled);
  gdk_event_free (event);
}

static void
emit_monitor_changed (Fixture *f)
{
  GFileMonitor *mon = g_object_get_data (G_OBJECT (f->popup), MON_KEY);
  gchar        *path = notify_db_path ();
  GFile        *file = g_file_new_for_path (path);

  g_assert_nonnull (mon);
  g_signal_emit_by_name (mon, "changed", file, NULL,
                         G_FILE_MONITOR_EVENT_CHANGED);
  g_object_unref (file);
  g_free (path);
}

/* --- linhas e navegação --- */

static void
test_rows_and_navigation (void)
{
  Fixture    f;
  sqlite3   *db;
  GtkWidget *row;
  GtkWidget *body;
  gint64     base = g_get_real_time ();

  if (skip_no_display ())
    return;
  db = db_create ();
  db_insert (db, "", "", "", base - 3 * G_USEC_PER_SEC);
  db_insert (db, "OnlyApp", "", "", base - 2 * G_USEC_PER_SEC);
  db_insert (db, "BodyApp", "", "Raw body", base - 1 * G_USEC_PER_SEC);
  db_insert (db, "App", "Sum", "Body", base);
  db_close (db);

  fixture_init (&f);
  gtk_widget_realize (f.popup);
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f.stack)),
                   ==, "notices");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 4);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "4");
  g_assert_true (gtk_widget_get_sensitive (f.qs->notify_clear));

  /* body vazio: título cai para "Notification" e a linha não expande. */
  row = find_row (f.qs->notify_list, "");
  g_assert_nonnull (row);
  g_assert_cmpstr (row_title (row), ==, "Notification");
  g_assert_null (row_body (row));
  click_row (row, GDK_BUTTON_PRIMARY);

  /* app sem summary/body: preview é o app e não é expandível. */
  row = find_row (f.qs->notify_list, "OnlyApp");
  g_assert_nonnull (row);
  g_assert_cmpstr (row_title (row), ==, "OnlyApp");
  g_assert_null (row_body (row));
  click_row (row, GDK_BUTTON_PRIMARY);

  /* summary vazio + body: título é o app e o preview é só o body. */
  row = find_row (f.qs->notify_list, "Raw body");
  g_assert_nonnull (row);
  g_assert_cmpstr (row_title (row), ==, "BodyApp");
  g_assert_nonnull (row_body (row));
  g_assert_true (gtk_list_box_row_get_activatable (GTK_LIST_BOX_ROW (row)));

  /* linha completa: preview "app • body", expande/recolhe só no primário. */
  row = find_row (f.qs->notify_list, "App \342\200\242 Body");
  g_assert_nonnull (row);
  g_assert_cmpstr (row_title (row), ==, "Sum");
  body = row_body (row);
  g_assert_nonnull (body);
  g_assert_cmpint (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                        EXPANDED_KEY)),
                   ==, 0);
  g_assert_cmpint (gtk_label_get_lines (GTK_LABEL (body)), ==, 1);
  g_assert_cmpint (gtk_label_get_ellipsize (GTK_LABEL (body)),
                   ==, PANGO_ELLIPSIZE_END);
  g_assert_cmpstr (gtk_widget_get_tooltip_text (row), ==, "Click to expand");

  click_row (row, GDK_BUTTON_PRIMARY);
  g_assert_cmpint (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                        EXPANDED_KEY)),
                   ==, 1);
  g_assert_cmpint (gtk_label_get_lines (GTK_LABEL (body)), ==, -1);
  g_assert_cmpint (gtk_label_get_ellipsize (GTK_LABEL (body)),
                   ==, PANGO_ELLIPSIZE_NONE);
  g_assert_cmpstr (gtk_widget_get_tooltip_text (row), ==, "Click to collapse");

  click_row (row, GDK_BUTTON_MIDDLE);
  g_assert_cmpint (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                        EXPANDED_KEY)),
                   ==, 1);
  click_row (row, GDK_BUTTON_SECONDARY);
  g_assert_cmpint (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                        EXPANDED_KEY)),
                   ==, 1);

  click_row (row, GDK_BUTTON_PRIMARY);
  g_assert_cmpint (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                        EXPANDED_KEY)),
                   ==, 0);
  g_assert_cmpint (gtk_label_get_lines (GTK_LABEL (body)), ==, 1);

  /* voltar atualiza o tile e retorna para a página principal. */
  g_signal_emit_by_name (f.back, "clicked");
  g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (f.stack)),
                   ==, "main");

  /* clear: apaga o sqlite, mostra placeholder e zera o subtítulo. */
  g_signal_emit_by_name (f.tile, "clicked");
  g_signal_emit_by_name (f.qs->notify_clear, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==,
                   "Empty");
  g_assert_false (gtk_widget_get_sensitive (f.qs->notify_clear));
  {
    GList *notices = list_notices ();

    g_assert_null (notices);
    g_list_free_full (notices, notice_free);
  }

  fixture_destroy (&f);
}

/* --- debounce visível/oculto/morto --- */

static void
test_debounce_visible (void)
{
  Fixture      f;
  sqlite3     *db;
  guint        first;
  guint        second;

  if (skip_no_display ())
    return;
  db = db_create ();
  db_insert (db, "App", "One", "Body", g_get_real_time ());
  db_close (db);

  fixture_init (&f);
  gtk_widget_show_all (f.popup);
  gtk_stack_set_visible_child_name (GTK_STACK (f.stack), "notices");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpuint (timer_id (f.popup, POLL_KEY), !=, 0);

  /* Duas mudanças no diretório coalescem em um único timeout de 500ms. */
  {
    gchar *path = notify_db_path ();

    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    db_insert (db, "App", "Two", "Body", g_get_real_time ());
    db_close (db);
    g_free (path);
  }
  emit_monitor_changed (&f);
  first = timer_id (f.popup, DEB_KEY);
  g_assert_cmpuint (first, !=, 0);
  emit_monitor_changed (&f);
  second = timer_id (f.popup, DEB_KEY);
  g_assert_cmpuint (second, ==, first);

  run_loop_ms (700);
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), ==, 0);
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 2);

  /* Hide cancela o debounce pendente e o poll. */
  {
    gchar *path = notify_db_path ();

    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    db_insert (db, "App", "Three", "Body", g_get_real_time ());
    db_close (db);
    g_free (path);
  }
  emit_monitor_changed (&f);
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), !=, 0);
  gtk_widget_hide (f.popup);
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), ==, 0);
  g_assert_cmpuint (timer_id (f.popup, POLL_KEY), ==, 0);
  run_loop_ms (650);
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 2);

  /* Visível, mas oculto antes do disparo: refresh_visible é no-op. */
  gtk_widget_show_all (f.popup);
  gtk_stack_set_visible_child_name (GTK_STACK (f.stack), "notices");
  {
    gchar *path = notify_db_path ();

    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    db_insert (db, "App", "Four", "Body", g_get_real_time ());
    db_close (db);
    g_free (path);
  }
  emit_monitor_changed (&f);
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), !=, 0);
  {
    guint sid = g_signal_lookup ("hide", G_OBJECT_TYPE (f.popup));

    g_signal_handlers_block_matched (G_OBJECT (f.popup),
                                     G_SIGNAL_MATCH_ID, sid, 0, NULL, NULL,
                                     NULL);
    gtk_widget_hide (f.popup);
    run_loop_ms (650);
    g_signal_handlers_unblock_matched (G_OBJECT (f.popup),
                                       G_SIGNAL_MATCH_ID, sid, 0, NULL, NULL,
                                       NULL);
  }
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), ==, 0);
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 2);

  /* Dead antes do disparo: refresh_visible também é no-op. */
  gtk_widget_show_all (f.popup);
  gtk_stack_set_visible_child_name (GTK_STACK (f.stack), "notices");
  {
    gchar *path = notify_db_path ();

    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    db_insert (db, "App", "Five", "Body", g_get_real_time ());
    db_close (db);
    g_free (path);
  }
  emit_monitor_changed (&f);
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), !=, 0);
  f.qs->dead = TRUE;
  run_loop_ms (650);
  f.qs->dead = FALSE;
  g_assert_cmpuint (timer_id (f.popup, DEB_KEY), ==, 0);
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 2);

  fixture_destroy (&f);
}

/* --- polling de 3s --- */

static void
test_poll_cycle (void)
{
  Fixture *f;
  sqlite3 *db;
  guint    first;

  if (skip_no_display ())
    return;
  db = db_create ();
  db_insert (db, "App", "One", "Body", g_get_real_time ());
  db_close (db);

  f = g_new0 (Fixture, 1);
  fixture_init (f);
  gtk_widget_show_all (f->popup);
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "notices");
  first = timer_id (f->popup, POLL_KEY);
  g_assert_cmpuint (first, !=, 0);

  /* show de novo não duplica o timer. */
  g_signal_emit_by_name (f->popup, "show");
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, first);

  /* tick após ~3s atualiza lista/tile e mantém o poll. O monitor do
   * diretório fica bloqueado para o rebuild vir só do tick. */
  {
    GFileMonitor *mon = g_object_get_data (G_OBJECT (f->popup), MON_KEY);
    guint         sid = g_signal_lookup ("changed", G_TYPE_FILE_MONITOR);
    gchar        *path = notify_db_path ();

    g_assert_nonnull (mon);
    g_signal_handlers_block_matched (G_OBJECT (mon), G_SIGNAL_MATCH_ID, sid,
                                     0, NULL, NULL, NULL);
    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    db_insert (db, "App", "Two", "Body", g_get_real_time ());
    db_close (db);
    g_assert_cmpuint (list_count (f->qs->notify_list), ==, 1);
    wait_list_count (f->qs->notify_list, 2, 4500);
    g_signal_handlers_unblock_matched (G_OBJECT (mon), G_SIGNAL_MATCH_ID,
                                       sid, 0, NULL, NULL, NULL);
    g_free (path);
  }
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), !=, 0);
  g_assert_cmpuint (list_count (f->qs->notify_list), ==, 2);
  wait_label (f->qs->notify_sub, "2", 1000);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f->qs->notify_sub)), ==, "2");

  /* sair da página para o poll; voltar religa. */
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "main");
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "notices");
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), !=, 0);

  /* hide para o poll. */
  gtk_widget_hide (f->popup);
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);

  /* popup morto não inicia poll, mesmo com a página certa visível. */
  f->qs->dead = TRUE;
  gtk_widget_show_all (f->popup);
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);
  f->qs->dead = FALSE;
  gtk_widget_hide (f->popup);
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);

  /* popup invisível não inicia poll ao entrar na página. */
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "main");
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "notices");
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);

  /* página principal visível também não inicia poll. */
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "main");
  gtk_widget_show_all (f->popup);
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), ==, 0);
  gtk_stack_set_visible_child_name (GTK_STACK (f->stack), "notices");
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), !=, 0);

  /* destroy com poll e debounce ativos cancela tudo. */
  g_assert_cmpuint (timer_id (f->popup, POLL_KEY), !=, 0);
  emit_monitor_changed (f);
  g_assert_cmpuint (timer_id (f->popup, DEB_KEY), !=, 0);
  g_assert_nonnull (g_object_get_data (G_OBJECT (f->popup), MON_KEY));
  fixture_destroy (f);
  g_free (f);
}

/* --- cache do tile e transições de lista --- */

static void
test_fill_changed_edges (void)
{
  Fixture    f;
  sqlite3   *db;
  GtkWidget *before;
  GtkWidget *after;
  gchar     *path;

  if (skip_no_display ())
    return;
  db = db_create ();
  db_insert (db, "App", "One", "Body", g_get_real_time () - 3000000);
  db_insert (db, "App", "Two", "Body", g_get_real_time () - 2000000);
  db_insert (db, "App", "Three", "Body", g_get_real_time () - 1000000);
  db_close (db);

  fixture_init (&f);
  notify_update_tile (f.qs);
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 3);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "3");
  before = row_at (f.qs->notify_list, 0);

  /* stat igual e lista sincronizada: fast-path sem rebuild. */
  g_signal_emit_by_name (f.tile, "clicked");
  after = row_at (f.qs->notify_list, 0);
  g_assert_true (before == after);
  notify_update_tile (f.qs);
  notify_update_tile (f.qs);

  /* count/max iguais mas mtime diferente: atualiza só o tile. */
  path = notify_db_path ();
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
                                 "UPDATE notifications SET app_name='New' "
                                 "WHERE summary='One';",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
  db_close (db);
  notify_update_tile (f.qs);
  g_signal_emit_by_name (f.tile, "clicked");
  after = row_at (f.qs->notify_list, 0);
  g_assert_true (before == after);

  /* Probe async atualiza o cache do tile para 4 sem tocar na lista. */
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  db_insert (db, "App", "Four", "Body", g_get_real_time ());
  db_close (db);
  notify_refresh_tile_async (f.qs);
  wait_label (f.qs->notify_sub, "4", 3000);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "4");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 3);

  /* stat igual ao cache mas lista dessincronizada: rebuild sem sqlite. */
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 4);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "4");

  /* DB sumiu: update_tile pinta o cache stale; lista vira placeholder. */
  g_remove (path);
  notify_update_tile (f.qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "4");
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==,
                   "Empty");

  /* Arquivo existe mas não é sqlite: assinatura falha com stat válido. */
  g_file_set_contents (path, "not a database", -1, NULL);
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==,
                   "Empty");
  /* Mesmo stat e lista vazia: volta ao fast-path. */
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);

  /* Clear com sqlite inacessível (diretório): fecha o handle e refaz vazio. */
  g_remove (path);
  g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
  g_signal_emit_by_name (f.qs->notify_clear, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==,
                   "Empty");
  g_assert_cmpint (g_rmdir (path), ==, 0);

  /* DB recriado com 1 linha: preenche de novo. */
  db = db_create ();
  db_insert (db, "App", "Again", "Body", g_get_real_time ());
  db_close (db);
  g_signal_emit_by_name (f.tile, "clicked");
  g_assert_cmpuint (list_count (f.qs->notify_list), ==, 1);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (f.qs->notify_sub)), ==, "1");
  g_free (path);

  fixture_destroy (&f);
}

/* --- probe async: sucesso, DB ilegível, seq velha e dead --- */

static void
test_async_probe_edges (void)
{
  QuickSettings *qs;
  sqlite3       *db;
  gchar         *path;

  if (skip_no_display ())
    return;
  db = db_create ();
  db_insert (db, "App", "One", "Body", g_get_real_time ());
  db_insert (db, "App", "Two", "Body", g_get_real_time ());
  db_close (db);

  qs = g_new0 (QuickSettings, 1);
  qs->refcount = 1;
  qs->notify_sub = gtk_label_new ("…");

  notify_refresh_tile_async (NULL);
  qs->dead = TRUE;
  notify_refresh_tile_async (qs);
  qs->dead = FALSE;
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "…");

  notify_refresh_tile_async (qs);
  wait_label (qs->notify_sub, "2", 3000);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "2");

  /* seq incrementada antes do idle: resultado velho é descartado. */
  path = notify_db_path ();
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  db_insert (db, "App", "Three", "Body", g_get_real_time ());
  db_close (db);
  notify_refresh_tile_async (qs);
  qs->cache_seq++;
  run_loop_ms (500);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "2");

  /* DB é diretório: stat ok, assinatura falha (cache de stat registrado). */
  g_remove (path);
  g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
  notify_refresh_tile_async (qs);
  run_loop_ms (500);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "2");
  notify_update_tile (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "2");
  g_rmdir (path);

  /* dead antes do idle: nada é aplicado. */
  db = db_create ();
  db_insert (db, "App", "Four", "Body", g_get_real_time ());
  db_close (db);
  notify_refresh_tile_async (qs);
  qs->dead = TRUE;
  run_loop_ms (500);
  qs->dead = FALSE;
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->notify_sub)), ==, "2");

  qs_cache_stop ();
  g_free (path);
  gtk_widget_destroy (qs->notify_sub);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-notify-flow-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  /* XDG antes de gtk_init: o GLib cacheia os diretórios do usuário. */
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/notify-flow/rows_navigation", test_rows_and_navigation);
  g_test_add_func ("/notify-flow/debounce_visible", test_debounce_visible);
  g_test_add_func ("/notify-flow/poll_cycle", test_poll_cycle);
  g_test_add_func ("/notify-flow/fill_changed_edges",
                   test_fill_changed_edges);
  g_test_add_func ("/notify-flow/async_probe_edges", test_async_probe_edges);
  return g_test_run ();
}
