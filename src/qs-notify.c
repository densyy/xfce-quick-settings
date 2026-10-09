#include "qs-notify.h"
#include "qs-ui.h"
#include "qs-util.h"

#define NOTIFY_MAX_ROWS 30

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

/* Path to the xfce4-notifyd history database (may not exist). */
static gchar *
notify_db_path (void)
{
  return g_build_filename (g_get_home_dir (), ".cache", "xfce4",
                           "notifyd", "log.sqlite", NULL);
}

/* Newest first, or NULL when sqlite3/the log is missing. */
static GList *
list_notices (void)
{
  gchar  *path = notify_db_path ();
  gchar  *quoted;
  gchar  *cmd;
  gchar  *output;
  gchar **lines;
  GList  *notices = NULL;
  gint    i;

  if (!command_exists ("sqlite3")
      || !g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_free (path);
      return NULL;
    }
  quoted = g_shell_quote (path);
  cmd = g_strdup_printf ("sqlite3 -separator '\x1f' %s \"SELECT app_name, "
                         "summary, body, timestamp FROM notifications "
                         "ORDER BY timestamp DESC LIMIT %d;\"",
                         quoted, NOTIFY_MAX_ROWS);
  g_free (quoted);
  g_free (path);
  output = run_command (cmd);
  g_free (cmd);
  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  g_free (output);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar  **fields;
      Notice  *n;

      if (lines[i][0] == '\0')
        continue;
      fields = g_strsplit (lines[i], "\x1f", 4);
      if (g_strv_length (fields) < 4)
        {
          g_strfreev (fields);
          continue;
        }
      n = g_new0 (Notice, 1);
      n->app = g_strdup (fields[0]);
      n->summary = g_strdup (fields[1]);
      n->body = g_strdup (fields[2]);
      n->timestamp = g_ascii_strtoll (fields[3], NULL, 10);
      g_strfreev (fields);
      notices = g_list_append (notices, n);
    }
  g_strfreev (lines);
  return notices;
}

/* "14:32" for today, "09/10 14:32" otherwise. */
static gchar *
notice_time (gint64 timestamp)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (timestamp);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  gint year, month, day;

  if (dt == NULL)
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
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title;
  GtkWidget *sub;
  gchar     *esc;
  gchar     *markup;
  gchar     *time;
  gchar     *sub_text;

  esc = g_markup_escape_text (n->summary != NULL && *n->summary != '\0'
                              ? n->summary : n->app, -1);
  markup = g_strdup_printf ("<b>%s</b>", *esc != '\0' ? esc : "Notification");
  title = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (title), markup);
  g_free (markup);
  g_free (esc);
   gtk_widget_set_halign (title, GTK_ALIGN_START);
   gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
   gtk_box_pack_start (GTK_BOX (vbox), title, FALSE, FALSE, 0);

   if (n->body != NULL && *n->body != '\0')
     {
       GtkWidget *body = gtk_label_new (NULL);

       esc = g_markup_escape_text (n->body, -1);
       markup = g_strdup_printf ("<small>%s</small>", esc);
       gtk_label_set_markup (GTK_LABEL (body), markup);
       g_free (markup);
       g_free (esc);
       gtk_widget_set_halign (body, GTK_ALIGN_START);
       gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
       gtk_label_set_lines (GTK_LABEL (body), 3);
       gtk_label_set_ellipsize (GTK_LABEL (body), PANGO_ELLIPSIZE_END);
       gtk_box_pack_start (GTK_BOX (vbox), body, FALSE, FALSE, 0);
     }

   time = notice_time (n->timestamp);
  sub_text = g_strdup_printf ("%s · %s", n->app, time);
  esc = g_markup_escape_text (sub_text, -1);
  markup = g_strdup_printf ("<small>%s</small>", esc);
  sub = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (sub), markup);
  g_free (markup);
  g_free (esc);
  g_free (sub_text);
  g_free (time);
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

  clear_list_box (qs->notify_list);
  if (notices == NULL)
    {
      gtk_list_box_insert (GTK_LIST_BOX (qs->notify_list),
                           make_placeholder_row ("No notifications yet"), -1);
    }
  else
    {
      for (l = notices; l != NULL; l = l->next)
        gtk_list_box_insert (GTK_LIST_BOX (qs->notify_list),
                             make_notice_row (l->data), -1);
      g_list_free_full (notices, notice_free);
    }
  gtk_widget_set_sensitive (qs->notify_clear, notices != NULL);
  gtk_widget_show_all (qs->notify_list);
}

static void
on_notify_clear_clicked (GtkButton *button,
                         gpointer   user_data)
{
  QuickSettings *qs = user_data;
  gchar *path = notify_db_path ();
  gchar *quoted;
  gchar *cmd;
  gchar *output;

  (void) button;
  if (command_exists ("sqlite3")
      && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      quoted = g_shell_quote (path);
      cmd = g_strdup_printf ("sqlite3 %s \"DELETE FROM notifications;\"",
                             quoted);
      g_free (quoted);
      output = run_command (cmd);
      g_free (cmd);
      g_free (output);
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
  (void) button;
  show_main_page (user_data);
}

void
notify_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *notify_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *notify_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *notify_tile;
  GtkWidget *page;
  GtkWidget *scroll;
  GtkWidget *back;

  /* Full-width tile above the Power tile. */
  gtk_box_pack_start (GTK_BOX (box), notify_box, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (notify_box), notify_sep, FALSE, FALSE, 2);
  notify_tile = make_tile (notify_box, "preferences-system-notifications-symbolic",
                            "Notifications");
  g_signal_connect (notify_tile, "clicked",
                    G_CALLBACK (on_notify_tile_clicked), qs);

  /* Sub-page with the scrollable history. */
  page = make_sub_page ("Notifications", &back);
  scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scroll, -1, 280);
  qs->notify_list = gtk_list_box_new ();
  gtk_container_add (GTK_CONTAINER (scroll), qs->notify_list);
  gtk_box_pack_start (GTK_BOX (page), scroll, TRUE, TRUE, 0);
  qs->notify_clear = gtk_button_new_with_label ("Clear");
  gtk_box_pack_start (GTK_BOX (page), qs->notify_clear, FALSE, FALSE, 0);
  g_signal_connect (qs->notify_clear, "clicked",
                    G_CALLBACK (on_notify_clear_clicked), qs);
  gtk_stack_add_named (GTK_STACK (stack), page, "notices");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_notify_back_clicked), qs);
}
