#include "qs-notify.h"
#include "qs-notify-store.h"
#include "qs-cache.h"
#include "qs-ui.h"

#include <sqlite3.h>
#include <sys/stat.h>

/* Live refresh: dir monitor (sqlite uses rename/journal, so the file
 * itself can't be watched reliably) + 3s poll fallback while visible. */
#define NOTIFY_POLL_S 3
#define NOTIFY_DEBOUNCE_MS 500
#define NOTIFY_DATA_MON "qs-notify-monitor"
#define NOTIFY_DATA_POLL "qs-notify-poll"
#define NOTIFY_DATA_DEB "qs-notify-debounce"

/* ---- cache estático interno: evita COUNT(*)/rebuild sem mudança ----
 * Tile usa stat(mtime+size) como fast-path: sem sqlite quando o arquivo
 * não mudou. Lista só reconstrói 30 rows quando count/max(timestamp)
 * diverge do último fill. Poll só com a página "notices" visível
 * (start/stop no visible-child + show/hide). */
static gint notify_cached_count = -1;
static gint64 notify_cached_max = -1;
static time_t notify_cached_mtime = 0;
static off_t notify_cached_size = 0;
static gboolean notify_cache_ready = FALSE;
static gint notify_list_count = -1;
static gint64 notify_list_max = -1;
static gint notify_last_count = G_MININT;

static void
notify_set_tile_count (QuickSettings *qs,
                       gint           count)
{
  if (count == notify_last_count)
    return;
  notify_last_count = count;
  if (count <= 0)
    gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
  else
    {
      gchar *text = g_strdup_printf ("%d", count);

      gtk_label_set_text (GTK_LABEL (qs->notify_sub), text);
      g_free (text);
    }
}

/* Collapsed/expanded state for compact notice rows.
 * Each expandable row stores its single preview label ("app • body")
 * under NOTICE_BODY_KEY and a 0/1 flag under NOTICE_EXPANDED_KEY.
 * Only "button-press-event" toggles (not "activate"): a click would
 * otherwise fire both and cancel itself out. */
#define NOTICE_BODY_KEY "qs-notice-body"
#define NOTICE_EXPANDED_KEY "qs-notice-expanded"

static void
notice_row_set_expanded (GtkWidget *row,
                         gboolean   expanded)
{
  GtkWidget *body = g_object_get_data (G_OBJECT (row), NOTICE_BODY_KEY);

  if (body == NULL)
    return;
  if (expanded)
    {
      gtk_label_set_lines (GTK_LABEL (body), -1);
      gtk_label_set_ellipsize (GTK_LABEL (body), PANGO_ELLIPSIZE_NONE);
      gtk_widget_set_tooltip_text (row, "Click to collapse");
    }
  else
    {
      gtk_label_set_lines (GTK_LABEL (body), 1);
      gtk_label_set_ellipsize (GTK_LABEL (body), PANGO_ELLIPSIZE_END);
      gtk_widget_set_tooltip_text (row, "Click to expand");
    }
  g_object_set_data (G_OBJECT (row), NOTICE_EXPANDED_KEY,
                     GUINT_TO_POINTER (expanded ? 1 : 0));
}

static void
notice_row_toggle (GtkWidget *row)
{
  gboolean expanded;

  if (g_object_get_data (G_OBJECT (row), NOTICE_BODY_KEY) == NULL)
    return;
  expanded = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row),
                                                  NOTICE_EXPANDED_KEY)) != 0;
  notice_row_set_expanded (row, !expanded);
}

static gboolean
on_notice_row_button_press (GtkWidget      *row,
                            GdkEventButton *event,
                            gpointer        user_data)
{
  (void) user_data;
  if (event != NULL && event->button != GDK_BUTTON_PRIMARY)
    return FALSE;
  notice_row_toggle (row);
  return FALSE;
}

/* Compact collapsed row: line 1 = bold summary (1 line, ellipsis) + time
 * pinned right; line 2 = "app • body" truncated to 1 line with ellipsis
 * (app alone when there is no body). Clicking an expandable row toggles
 * the full wrapped body. Timestamp/counter/Clear/refresh live elsewhere
 * and are untouched. */
static GtkWidget *
make_notice_row (Notice *n)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL,
                                 QS_TIGHT_GAP / 2);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                   QS_INNER_GAP);
  GtkWidget *title;
  GtkWidget *time_label;
  GtkWidget *preview;
  gchar     *time;
  gchar     *preview_text;
  const gchar *title_text;
  gboolean   has_body;
  gboolean   title_is_app;

  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  /* Tighter than QS_CONTENT_PAD (10) so a collapsed row stays ~2 lines. */
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 6);

  /* Bold summary with the time pinned to the right.
   * Plain text (no markup escape): GtkLabel shows text literally,
   * so pre-escaped "&amp;" would leak to the UI. */
  title_text = (n->summary != NULL && *n->summary != '\0')
               ? n->summary
               : (n->app != NULL && *n->app != '\0' ? n->app : "Notification");
  title_is_app = (g_strcmp0 (title_text, n->app) == 0);
  title = gtk_label_new (title_text);
  ui_add_class (title, "qs-title");
  gtk_widget_set_halign (title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_lines (GTK_LABEL (title), 1);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  time = notice_time (n->timestamp);
  time_label = gtk_label_new (time);
  ui_add_class (time_label, "qs-sub");
  g_free (time);
  gtk_box_pack_start (GTK_BOX (header), title, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (header), time_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), header, FALSE, FALSE, 0);

  has_body = (n->body != NULL && *n->body != '\0');
  if (has_body && !title_is_app && n->app != NULL && *n->app != '\0')
    preview_text = g_strdup_printf ("%s \342\200\242 %s", n->app, n->body);
  else if (has_body)
    preview_text = g_strdup (n->body);
  else
    preview_text = g_strdup (n->app != NULL ? n->app : "");
  preview = gtk_label_new (preview_text);
  g_free (preview_text);
  ui_add_class (preview, "qs-sub");
  gtk_widget_set_halign (preview, GTK_ALIGN_START);
  gtk_widget_set_hexpand (preview, TRUE);
  gtk_label_set_line_wrap (GTK_LABEL (preview), TRUE);
  gtk_label_set_lines (GTK_LABEL (preview), 1);
  gtk_label_set_ellipsize (GTK_LABEL (preview), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (vbox), preview, FALSE, FALSE, 0);

  if (has_body)
    {
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data (G_OBJECT (row), NOTICE_BODY_KEY, preview);
      g_object_set_data (G_OBJECT (row), NOTICE_EXPANDED_KEY,
                         GUINT_TO_POINTER (0));
      gtk_widget_set_tooltip_text (row, "Click to expand");
      g_signal_connect (row, "button-press-event",
                        G_CALLBACK (on_notice_row_button_press), NULL);
    }

  gtk_container_add (GTK_CONTAINER (row), vbox);
  return row;
}

static void
notify_fill_list (QuickSettings *qs)
{
  GList *notices = list_notices ();
  GList *l;
  guint  count = 0;
  gint64 max_ts = 0;

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
          Notice *n = l->data;

          if (count == 0)
            max_ts = n->timestamp;
          else if (n->timestamp > max_ts)
            max_ts = n->timestamp;
          gtk_list_box_insert (GTK_LIST_BOX (qs->notify_list),
                               make_notice_row (n), -1);
          count++;
        }
      g_list_free_full (notices, notice_free);
    }
  notify_set_tile_count (qs, (gint) count);
  gtk_widget_set_sensitive (qs->notify_clear, count > 0);
  gtk_widget_show_all (qs->notify_list);
  notify_cached_count = (gint) count;
  notify_cached_max = max_ts;
  notify_cache_ready = TRUE;
  notify_list_count = (gint) count;
  notify_list_max = max_ts;
  {
    gchar *path = notify_db_path ();
    struct stat st;

    if (stat (path, &st) == 0)
      {
        notify_cached_mtime = st.st_mtime;
        notify_cached_size = st.st_size;
      }
    g_free (path);
  }
}

/* Reconstrói 30 rows só se count/max(timestamp) mudou desde o último
 * fill. Fast-path via stat: sem sqlite quando mtime+size iguais e a
 * lista já está sincronizada. */
static void
notify_fill_list_if_changed (QuickSettings *qs)
{
  gchar      *path = notify_db_path ();
  struct stat st;
  gboolean    have_stat = (stat (path, &st) == 0);
  gboolean    stat_same;

  g_free (path);
  stat_same = (notify_cache_ready && have_stat
               && st.st_mtime == notify_cached_mtime
               && st.st_size == notify_cached_size);
  if (stat_same && notify_list_count == notify_cached_count
      && notify_list_max == notify_cached_max)
    return;
  if (stat_same)
    {
      /* DB igual ao cache mas lista dessincronizada (ex.: tile atualizou
       * na main e a lista ainda não): precisa rebuild mesmo sem sqlite. */
      if (notify_list_count == notify_cached_count
          && notify_list_max == notify_cached_max)
        return;
      notify_fill_list (qs);
      return;
    }
  {
    gint   count = -1;
    gint64 max_ts = 0;

    if (!notify_db_signature (&count, &max_ts))
      {
        /* DB sumiu/ilegível: mostra vazio e invalida só o necessário. */
        if (notify_list_count != 0 || notify_list_max != 0)
          notify_fill_list (qs);
        else
          notify_set_tile_count (qs, 0);
        notify_cached_count = 0;
        notify_cached_max = 0;
        notify_cache_ready = TRUE;
        if (have_stat)
          {
            notify_cached_mtime = st.st_mtime;
            notify_cached_size = st.st_size;
          }
        return;
      }
    if (count == notify_list_count && max_ts == notify_list_max)
      {
        /* Só atualiza tile/stat, sem reconstruir 30 rows. */
        notify_cached_count = count;
        notify_cached_max = max_ts;
        notify_cache_ready = TRUE;
        if (have_stat)
          {
            notify_cached_mtime = st.st_mtime;
            notify_cached_size = st.st_size;
          }
        notify_set_tile_count (qs, count);
        return;
      }
    notify_cached_count = count;
    notify_cached_max = max_ts;
    notify_cache_ready = TRUE;
    if (have_stat)
      {
        notify_cached_mtime = st.st_mtime;
        notify_cached_size = st.st_size;
      }
    notify_fill_list (qs);
  }
}

/* Tile leve cached/fast-path apenas (T5): stat mtime+size + tile.
 * NUNCA sqlite aqui — a correção vai em notify_refresh_tile_async. */
void
notify_update_tile (QuickSettings *qs)
{
  gchar      *path = notify_db_path ();
  struct stat st;
  gboolean    have_stat = (stat (path, &st) == 0);

  g_free (path);
  if (notify_cache_ready && have_stat
      && st.st_mtime == notify_cached_mtime
      && st.st_size == notify_cached_size)
    {
      notify_set_tile_count (qs, notify_cached_count);
      return;
    }
  if (notify_cache_ready)
    {
      /* Stat divergiu (ou DB sumiu): pinta o último valor conhecido
       * (stale); o worker async corrige em seguida. */
      notify_set_tile_count (qs, notify_cached_count);
      return;
    }
  /* First-run sem cache: mantém o que qs_cache_load pintou ("…"/"Empty"). */
}

typedef struct
{
  QuickSettings *qs;
  gint           count;
  gint64         max_ts;
  struct stat    st;
  gboolean       have_stat;
  gboolean       ok;
  guint          seq;
} NotifyTileProbe;

static gboolean
notify_tile_probe_idle (gpointer data)
{
  NotifyTileProbe *p = data;
  QuickSettings   *qs = p->qs;

  if (!g_atomic_int_get (&qs->dead) && p->seq == qs->cache_seq && p->ok)
    {
      notify_cached_count = p->count;
      notify_cached_max = p->max_ts;
      notify_cache_ready = TRUE;
      if (p->have_stat)
        {
          notify_cached_mtime = p->st.st_mtime;
          notify_cached_size = p->st.st_size;
        }
      notify_set_tile_count (qs, p->count);
      qs_cache_schedule_save (qs);
    }
  else if (!g_atomic_int_get (&qs->dead) && p->seq == qs->cache_seq && !p->ok && p->have_stat)
    {
      /* DB ilegível mas stat ok: registra stat p/ não repetir sqlite. */
      notify_cached_mtime = p->st.st_mtime;
      notify_cached_size = p->st.st_size;
    }
  g_free (p);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static gpointer
notify_tile_probe_thread (gpointer data)
{
  NotifyTileProbe *p = data;
  gchar           *path = notify_db_path ();

  /* Worker: SÓ stat + notify_db_signature (plain-data). NUNCA gtk_*,
   * NUNCA handle sqlite3* entre threads (só count/max_ts copiados). */
  p->have_stat = (stat (path, &p->st) == 0);
  g_free (path);
  p->ok = notify_db_signature (&p->count, &p->max_ts);
  g_idle_add (notify_tile_probe_idle, p);
  return NULL;
}

/* Correção assíncrona (T5): worker stat+sqlite, volta na main via idle.
 * Reaproveita o debounce existente (NOTIFY_DEBOUNCE_MS) via o caminho
 * notify_schedule_refresh → notify_refresh_visible; sem segundo timer. */
void
notify_refresh_tile_async (QuickSettings *qs)
{
  NotifyTileProbe *p;

  if (qs == NULL || g_atomic_int_get (&qs->dead))
    return;
  p = g_new0 (NotifyTileProbe, 1);
  p->qs = qs;
  p->seq = ++qs->cache_seq;
  qs_ref (qs);
  {
    GThread *t = g_thread_try_new ("qs-notify-tile",
                                   notify_tile_probe_thread, p, NULL);

    if (t == NULL)
      {
        qs_unref (qs);
        g_free (p);
      }
    else
      g_thread_unref (t);
  }
}

/* Refreshes tile always, list only when the "notices" page is showing
 * e só se count/timestamp mudou. No-ops while hidden/dead. */
static void
notify_refresh_visible (QuickSettings *qs)
{
  const gchar *name;

  if (g_atomic_int_get (&qs->dead) || !gtk_widget_get_visible (qs->popup))
    return;
  notify_update_tile (qs);
  /* T5: cached acima (instantâneo) + correção async; debounce de 500ms
   * já existente (NOTIFY_DEBOUNCE_MS) reaproveitado — sem segundo timer. */
  notify_refresh_tile_async (qs);
  name = gtk_stack_get_visible_child_name (GTK_STACK (qs->stack));
  if (g_strcmp0 (name, "notices") == 0)
    notify_fill_list_if_changed (qs);
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
  if (g_atomic_int_get (&qs->dead) || !gtk_widget_get_visible (qs->popup))
    return;
  notify_schedule_refresh (qs);
}

/* Poll fallback: só roda com a página "notices" visível (e popup
 * visível); auto-remove ao sair da página/ocultar (backup). */
static gboolean
notify_poll_tick (gpointer data)
{
  QuickSettings *qs = data;
  const gchar   *name;

  if (g_atomic_int_get (&qs->dead) || !gtk_widget_get_visible (qs->popup))
    {
      g_object_set_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL,
                         GUINT_TO_POINTER (0));
      return G_SOURCE_REMOVE;
    }
  name = gtk_stack_get_visible_child_name (GTK_STACK (qs->stack));
  if (g_strcmp0 (name, "notices") != 0)
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
  guint        id;
  const gchar *name;

  if (g_atomic_int_get (&qs->dead)
      || g_object_get_data (G_OBJECT (qs->popup), NOTIFY_DATA_POLL) != NULL)
    return;
  if (!gtk_widget_get_visible (qs->popup))
    return;
  name = gtk_stack_get_visible_child_name (GTK_STACK (qs->stack));
  if (g_strcmp0 (name, "notices") != 0)
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
  if (g_strcmp0 (name, "notices") == 0 && !g_atomic_int_get (&qs->dead))
    {
      notify_fill_list_if_changed (qs);
      notify_poll_start (qs);
    }
  else
    notify_poll_stop (qs);
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
  notify_cached_mtime = 0;
  notify_cached_size = 0;
  notify_cache_ready = FALSE;
  notify_fill_list_if_changed (qs);
}

static void
on_notify_tile_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  notify_fill_list_if_changed (qs);
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

  notify_tile = make_mini_tile (box, "preferences-system-notifications-symbolic",
                                "Notifications", &qs->notify_sub);
  gtk_label_set_text (GTK_LABEL (qs->notify_sub), "Empty");
  g_signal_connect (notify_tile, "clicked",
                    G_CALLBACK (on_notify_tile_clicked), qs);

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
   * it from anywhere, not just the tile click above). Poll só aqui. */
  g_signal_connect (stack, "notify::visible-child-name",
                    G_CALLBACK (on_notify_stack_child), qs);
  /* Poll fallback só com "notices" visível (show checa a página; hide
   * para como power/sysmon); monitor cobre o tempo real do tile. */
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
