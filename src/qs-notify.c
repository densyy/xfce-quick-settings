#include "qs-notify.h"
#include "qs-ui.h"

#include <sqlite3.h>

#define NOTIFY_MAX_ROWS 30
/* Live refresh: dir monitor (sqlite uses rename/journal, so the file
 * itself can't be watched reliably) + 3s poll fallback while visible. */
#define NOTIFY_POLL_S 3
#define NOTIFY_DEBOUNCE_MS 500
#define NOTIFY_DATA_MON "qs-notify-monitor"
#define NOTIFY_DATA_POLL "qs-notify-poll"
#define NOTIFY_DATA_DEB "qs-notify-debounce"

/* One row from the xfce4-notifyd log. */
typedef struct
{
  gchar *app;
  gchar *summary;
  gchar *body;
  gint64 timestamp;
} Notice;

static void
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
static gchar *
notify_db_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "xfce4",
                           "notifyd", "log.sqlite", NULL);
}

/* Flattens newlines: the list splits rows on '\n', so embedded
 * newlines would corrupt parsing (fixed via SQL before, now in C). */
static gchar *
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
 * separator-quoting fragility ('\x1f' via CLI never matched the C
 * split, so the list was always empty). */
static GList *
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
                           /* No urgency filter: critical and normal appear alike. */
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
static gchar *
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

static GtkWidget *
make_notice_row (Notice *n)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                   QS_INNER_GAP);
  GtkWidget *title;
  GtkWidget *time_label;
  GtkWidget *sub;
  gchar     *time;
  gchar     *sub_text;
  const gchar *title_text;

  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), QS_CONTENT_PAD);

  /* Bold summary with the time pinned to the right.
   * Plain text (no markup escape): GtkLabel shows text literally,
   * so pre-escaped "&amp;" would leak to the UI. */
  title_text = (n->summary != NULL && *n->summary != '\0')
               ? n->summary
               : (n->app != NULL && *n->app != '\0' ? n->app : "Notification");
  title = gtk_label_new (title_text);
  ui_add_class (title, "qs-title");
  gtk_widget_set_halign (title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  time = notice_time (n->timestamp);
  time_label = gtk_label_new (time);
  ui_add_class (time_label, "qs-sub");
  g_free (time);
  gtk_box_pack_start (GTK_BOX (header), title, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (header), time_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), header, FALSE, FALSE, 0);

  if (n->body != NULL && *n->body != '\0')
    {
      GtkWidget *body = gtk_label_new (n->body);
      gtk_widget_set_halign (body, GTK_ALIGN_START);
      gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
      gtk_label_set_lines (GTK_LABEL (body), 3);
      gtk_label_set_ellipsize (GTK_LABEL (body), PANGO_ELLIPSIZE_END);
      gtk_box_pack_start (GTK_BOX (vbox), body, FALSE, FALSE, 0);
    }

  sub_text = g_strdup (n->app != NULL ? n->app : "");
  sub = gtk_label_new (sub_text);
  g_free (sub_text);
  ui_add_class (sub, "qs-sub");
  gtk_widget_set_halign (sub, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (vbox), sub, FALSE, FALSE, 0);

  gtk_container_add (GTK_CONTAINER (row), vbox);
  return row;
}

static void
notify_fill_list (QuickSettings *qs)
{
  GList *notices = list_notices ();
  GList *l;
  guint  count = 0;

  clear_list_box (qs->notify_list);
  if (notices == NULL)
    {
      gtk_list_box_insert (GTK_LIST_BOX (qs->notify_list),
                           make_placeholder_row ("No notifications yet"), -1);
    }
  else
    {
      for (l = notices; l != NULL; l = l->next)
        {
          gtk_list_box_insert (GTK_LIST_BOX (qs->notify_list),
                               make_notice_row (l->data), -1);
          count++;
        }
      g_list_free_full (notices, notice_free);
    }
  if (count == 0)
    gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
  else
    {
      gchar *text = g_strdup_printf ("%u", count);

      gtk_label_set_text (GTK_LABEL (qs->notify_sub), text);
      g_free (text);
    }
  gtk_widget_set_sensitive (qs->notify_clear, count > 0);
  gtk_widget_show_all (qs->notify_list);
}

/* Lightweight count for the main-page tile (no row parsing). */
void
notify_update_tile (QuickSettings *qs)
{
  gchar  *path = notify_db_path ();
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  gint    count = -1;

  if (g_file_test (path, G_FILE_TEST_EXISTS)
      && sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
    {
      sqlite3_busy_timeout (db, 1000);
      if (sqlite3_prepare_v2 (db, "SELECT COUNT(*) FROM notifications;",
                              -1, &stmt, NULL) == SQLITE_OK
          && sqlite3_step (stmt) == SQLITE_ROW)
        count = sqlite3_column_int (stmt, 0);
    }
  if (stmt != NULL)
    sqlite3_finalize (stmt);
  if (db != NULL)
    sqlite3_close (db);
  g_free (path);
  if (count <= 0)
    gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
  else
    {
      gchar *text = g_strdup_printf ("%d", count);

      gtk_label_set_text (GTK_LABEL (qs->notify_sub), text);
      g_free (text);
    }
}

/* Refreshes tile always, list only when the "notices" page is showing.
 * No-ops while the popup is hidden or the plugin is dead. */
static void
notify_refresh_visible (QuickSettings *qs)
{
  const gchar *name;

  if (qs->dead || !gtk_widget_get_visible (qs->popup))
    return;
  notify_update_tile (qs);
  name = gtk_stack_get_visible_child_name (GTK_STACK (qs->stack));
  if (g_strcmp0 (name, "notices") == 0)
    notify_fill_list (qs);
}

static gboolean
notify_debounce_fire (gpointer data)
{
  QuickSettings *qs = data;

  g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_DEB,
                     GUINT_TO_POINTER (0));
  notify_refresh_visible (qs);
  return G_SOURCE_REMOVE;
}

/* Coalesces bursts of monitor events (one sqlite commit emits several)
 * into a single refresh ~500ms after the first event. */
static void
notify_schedule_refresh (QuickSettings *qs)
{
  guint id;

  if (g_object_get_data (G_OBJECT (qs->popup), NOTIFY_DATA_DEB) != NULL)
    return;
  id = g_timeout_add (NOTIFY_DEBOUNCE_MS, notify_debounce_fire, qs);
  g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_DEB,
                     GUINT_TO_POINTER (id));
}

static void
notify_unschedule_refresh (QuickSettings *qs)
{
  gpointer p = g_object_get_data (G_OBJECT (qs->popup), NOTIFY_DATA_DEB);

  if (p != NULL)
    {
      g_source_remove (GPOINTER_TO_UINT (p));
      g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_DEB,
                         GUINT_TO_POINTER (0));
    }
}

static void
on_notify_dir_changed (GFileMonitor      *monitor,
                       GFile             *file,
                       GFile             *other,
                       GFileMonitorEvent  event,
                       gpointer           user_data)
{
  QuickSettings *qs = user_data;

  (void) monitor;
  (void) file;
  (void) other;
  (void) event;
  if (qs->dead || !gtk_widget_get_visible (qs->popup))
    return;
  notify_schedule_refresh (qs);
}

/* Poll fallback: only ever runs while the popup is visible (started on
 * "show", stopped on "hide"); self-removes as a backup. */
static gboolean
notify_poll_tick (gpointer data)
{
  QuickSettings *qs = data;

  if (qs->dead || !gtk_widget_get_visible (qs->popup))
    {
      g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL,
                         GUINT_TO_POINTER (0));
      return G_SOURCE_REMOVE;
    }
  notify_refresh_visible (qs);
  return G_SOURCE_CONTINUE;
}

static void
notify_poll_start (QuickSettings *qs)
{
  guint id;

  if (qs->dead
      || g_object_get_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL) != NULL)
    return;
  id = g_timeout_add_seconds (NOTIFY_POLL_S, notify_poll_tick, qs);
  g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL,
                     GUINT_TO_POINTER (id));
}

static void
notify_poll_stop (QuickSettings *qs)
{
  gpointer p = g_object_get_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL);

  if (p != NULL)
    {
      g_source_remove (GPOINTER_TO_UINT (p));
      g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL,
                         GUINT_TO_POINTER (0));
    }
}

static void
on_notify_stack_child (GObject    *stack,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  QuickSettings *qs = user_data;
  const gchar *name;

  (void) pspec;
  name = gtk_stack_get_visible_child_name (GTK_STACK (stack));
  if (g_strcmp0 (name, "notices") == 0 && !qs->dead)
    notify_fill_list (qs);
}

static void
on_notify_popup_show (GtkWidget *widget,
                      gpointer   user_data)
{
  (void) widget;
  notify_poll_start (user_data);
}

static void
on_notify_popup_hide (GtkWidget *widget,
                      gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) widget;
  notify_poll_stop (qs);
  notify_unschedule_refresh (qs);
}

/* Cancels monitor + sources before qs is freed (hide already stopped
 * the poll, but the monitor and a pending debounce outlive visibility). */
static void
on_notify_popup_destroy (GtkWidget *widget,
                         gpointer   user_data)
{
  QuickSettings *qs = user_data;
  GFileMonitor *mon;

  notify_poll_stop (qs);
  notify_unschedule_refresh (qs);
  mon = g_object_steal_data (G_OBJECT (widget), NOTIFY_DATA_MON);
  if (mon != NULL)
    {
      g_file_monitor_cancel (mon);
      g_object_unref (mon);
    }
}

static void
on_notify_clear_clicked (GtkButton *button,
                         gpointer   user_data)
{
  QuickSettings *qs = user_data;
  gchar *path = notify_db_path ();
  sqlite3 *db = NULL;

  (void) button;
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    {
      if (sqlite3_open (path, &db) == SQLITE_OK)
        {
          sqlite3_busy_timeout (db, 1000);
          sqlite3_exec (db, "DELETE FROM notifications;", NULL, NULL, NULL);
          sqlite3_close (db);
        }
      else if (db != NULL)
        sqlite3_close (db);
    }
  g_free (path);
  notify_fill_list (qs);
}

static void
on_notify_tile_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  notify_fill_list (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "notices");
}

static void
on_notify_back_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  notify_update_tile (qs);
  show_main_page (qs);
}

void
notify_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *notify_tile;
  GtkWidget *page;
  GtkWidget *scroll;
  GtkWidget *back;
  gchar *mon_dir;
  GFile *mon_file;
  GFileMonitor *mon;

  /* Mini tile in the shared action grid. */
  notify_tile = make_mini_tile (box, "preferences-system-notifications-symbolic",
                                "Notifications", &qs->notify_sub);
  gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
  g_signal_connect (notify_tile, "clicked",
                    G_CALLBACK (on_notify_tile_clicked), qs);

  /* Sub-page with the scrollable history. */
  page = make_sub_page ("Notifications", &back);
  scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scroll, -1, 280);
  qs->notify_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (qs->notify_list),
                                   GTK_SELECTION_NONE);
  ui_add_class (qs->notify_list, "qs-list");
  gtk_container_add (GTK_CONTAINER (scroll), qs->notify_list);
  gtk_box_pack_start (GTK_BOX (page), scroll, TRUE, TRUE, 0);
  qs->notify_clear = gtk_button_new_with_label ("Clear");
  ui_add_class (qs->notify_clear, "qs-btn");
  gtk_box_pack_start (GTK_BOX (page), qs->notify_clear, FALSE, FALSE, 0);
  g_signal_connect (qs->notify_clear, "clicked",
                    G_CALLBACK (on_notify_clear_clicked), qs);
  gtk_stack_add_named (GTK_STACK (stack), page, "notices");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_notify_back_clicked), qs);
  /* Refill whenever the history page becomes visible (covers entering
   * it from anywhere, not just the tile click above). */
  g_signal_connect (stack, "notify::visible-child-name",
                    G_CALLBACK (on_notify_stack_child), qs);
  /* Poll fallback runs only while the popup is open (same hide signal
   * that stops power/sysmon); monitor below covers the live part. */
  g_signal_connect (qs->popup, "show",
                    G_CALLBACK (on_notify_popup_show), qs);
  g_signal_connect (qs->popup, "hide",
                    G_CALLBACK (on_notify_popup_hide), qs);
  g_signal_connect (qs->popup, "destroy",
                    G_CALLBACK (on_notify_popup_destroy), qs);
  /* Watch the notifyd dir, not log.sqlite: sqlite commits land via
   * journal/rename, so the file identity changes underneath us. */
  mon_dir = g_build_filename (g_get_user_cache_dir (), "xfce4",
                              "notifyd", NULL);
  mon_file = g_file_new_for_path (mon_dir);
  mon = g_file_monitor_directory (mon_file, G_FILE_MONITOR_NONE, NULL, NULL);
  if (mon != NULL)
    {
      g_signal_connect (mon, "changed",
                        G_CALLBACK (on_notify_dir_changed), qs);
      g_object_set_data_full (G_OBJECT (qs->popup), NOTIFY_DATA_MON, mon,
                              g_object_unref);
    }
  g_object_unref (mon_file);
  g_free (mon_dir);
}
