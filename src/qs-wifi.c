/* qs-wifi.c — Wi-Fi backend (nmcli) + Wi-Fi page.
 * TU própria (Makefile SRC); fachada em qs-net.h (qs-net.c delega). */

#include "qs-core.h"
#include "qs-net.h"
#include "qs-ui.h"
#include "qs-util.h"

#include <string.h>

typedef struct
{
  gchar    *ssid;
  gint      signal;
  gboolean  secured;
  gboolean  active;
} WifiNet;

static void
wifi_net_free (gpointer data)
{
  WifiNet *net = data;

  g_free (net->ssid);
  g_free (net);
}

/* Local copy of the nmcli terse splitter (unescaped ':'; handles \: \\).
 * Kept here so the wifi backend does not depend on qs-util's
 * split_terse_line() (which exists only for nmcli output). */
static gchar **
wifi_split_terse (const gchar *line,
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

gboolean
wifi_available (void)
{
  return command_exists ("nmcli");
}

/* Synchronous radio query. WORKER THREADS ONLY (spawns nmcli); the UI
 * thread must use the cached qs->wifi_on or the load worker result. */
gboolean
wifi_enabled_sync (void)
{
  gchar    *output = run_command ("nmcli radio wifi");
  gboolean  on = FALSE;

  if (output != NULL)
    {
      on = g_str_has_prefix (g_strstrip (output), "enabled");
      g_free (output);
    }
  return on;
}

static void
wifi_set_enabled (gboolean on)
{
  gchar *cmd = g_strdup_printf ("nmcli radio wifi %s", on ? "on" : "off");

  spawn_command (cmd);
  g_free (cmd);
}

/* Parses "IN-USE,SSID,SIGNAL,SECURITY" terse output. Dedupes by SSID (one
 * row per SSID): keeps the strongest signal, marks active if any BSSID
 * is current. prepend+reverse keeps list order without O(n^2) append. */
static GList *
parse_wifi_list (const gchar *output)
{
  GList      *nets = NULL;
  GHashTable *seen;
  gchar     **lines;
  gint        i;

  if (output == NULL)
    return NULL;
  /* Keys are net->ssid owned by the WifiNet values: destroy neither on
   * table teardown (the GList owns them). */
  seen = g_hash_table_new (g_str_hash, g_str_equal);
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar    **fields;
      WifiNet   *existing;
      gint       signal;
      gboolean   secured;
      gboolean   active;

      if (*lines[i] == '\0')
        continue;
      fields = wifi_split_terse (lines[i], 4);
      if (*fields[1] == '\0')
        {
          g_strfreev (fields);
          continue;
        }
      signal = atoi (fields[2]);
      secured = !(g_strcmp0 (fields[3], "--") == 0 || *fields[3] == '\0');
      active = (g_strcmp0 (fields[0], "*") == 0);
      existing = g_hash_table_lookup (seen, fields[1]);
      if (existing == NULL)
        {
          WifiNet *net = g_new0 (WifiNet, 1);

          net->ssid = g_strdup (fields[1]);
          net->signal = signal;
          net->secured = secured;
          net->active = active;
          nets = g_list_prepend (nets, net);
          g_hash_table_insert (seen, net->ssid, net);
        }
      else
        {
          if (signal > existing->signal)
            {
              existing->signal = signal;
              existing->secured = secured;
            }
          existing->active = existing->active || active;
        }
      g_strfreev (fields);
    }
  g_strfreev (lines);
  g_hash_table_destroy (seen);
  return g_list_reverse (nets);
}

/* --- Saved-profile cache (avoids `nmcli connection show` on UI path) ---
 * Dialog path (row click) uses the cache only: unknown/stale SSIDs are
 * treated as unsaved (password prompt, the safe default). The connect
 * worker re-queries synchronously off the UI thread. TTL 60s. */

#define WIFI_SAVED_TTL_SECS 60

static GHashTable *wifi_saved_set = NULL;
static gint64      wifi_saved_mtime = 0;
static GMutex      wifi_saved_mutex;
static gboolean    wifi_saved_mutex_inited = FALSE;

static void
wifi_saved_ensure (void)
{
  if (!wifi_saved_mutex_inited)
    {
      g_mutex_init (&wifi_saved_mutex);
      wifi_saved_mutex_inited = TRUE;
    }
  if (wifi_saved_set == NULL)
    wifi_saved_set = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, NULL);
}

static gboolean
wifi_saved_cache_valid_locked (void)
{
  if (wifi_saved_mtime == 0 || wifi_saved_set == NULL)
    return FALSE;
  return (g_get_monotonic_time () - wifi_saved_mtime)
         < ((gint64) WIFI_SAVED_TTL_SECS * G_USEC_PER_SEC);
}

/* WORKER ONLY: rebuilds the set from `nmcli connection show`. Holds no
 * lock while spawning; swaps the table under lock. */
static void
wifi_saved_refresh_sync (void)
{
  gchar       *output = run_command ("nmcli -t -f NAME,TYPE connection show");
  GHashTable  *fresh;
  gchar      **lines;
  gint         i;

  wifi_saved_ensure ();
  fresh = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (output != NULL)
    {
      lines = g_strsplit (output, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          gchar **fields;

          if (*lines[i] == '\0')
            continue;
          fields = wifi_split_terse (lines[i], 2);
          if (g_strcmp0 (fields[0], "") != 0
              && g_strcmp0 (fields[1], "802-11-wireless") == 0
              && g_hash_table_lookup (fresh, fields[0]) == NULL)
            g_hash_table_insert (fresh, g_strdup (fields[0]),
                                 GINT_TO_POINTER (1));
          g_strfreev (fields);
        }
      g_strfreev (lines);
      g_free (output);
    }
  g_mutex_lock (&wifi_saved_mutex);
  if (wifi_saved_set != NULL)
    g_hash_table_destroy (wifi_saved_set);
  wifi_saved_set = fresh;
  wifi_saved_mtime = g_get_monotonic_time ();
  g_mutex_unlock (&wifi_saved_mutex);
}

/* UI-SAFE (no spawn): cached answer only; FALSE when stale/unknown. */
static gboolean
wifi_saved_lookup_cached (const gchar *ssid)
{
  gboolean saved = FALSE;

  wifi_saved_ensure ();
  g_mutex_lock (&wifi_saved_mutex);
  if (wifi_saved_cache_valid_locked () && ssid != NULL)
    saved = (g_hash_table_lookup (wifi_saved_set, ssid) != NULL);
  g_mutex_unlock (&wifi_saved_mutex);
  return saved;
}

/* WORKER ONLY: refresh-if-stale then lookup (may spawn nmcli). */
static gboolean
wifi_saved_query_sync (const gchar *ssid)
{
  gboolean valid;

  wifi_saved_ensure ();
  g_mutex_lock (&wifi_saved_mutex);
  valid = wifi_saved_cache_valid_locked ();
  if (valid && ssid != NULL)
    {
      gboolean hit = (g_hash_table_lookup (wifi_saved_set, ssid) != NULL);

      g_mutex_unlock (&wifi_saved_mutex);
      return hit;
    }
  g_mutex_unlock (&wifi_saved_mutex);
  wifi_saved_refresh_sync ();
  g_mutex_lock (&wifi_saved_mutex);
  valid = (ssid != NULL && g_hash_table_lookup (wifi_saved_set,
                                                ssid) != NULL);
  g_mutex_unlock (&wifi_saved_mutex);
  return valid;
}

static void
wifi_saved_invalidate (void)
{
  wifi_saved_ensure ();
  g_mutex_lock (&wifi_saved_mutex);
  wifi_saved_mtime = 0;
  g_mutex_unlock (&wifi_saved_mutex);
}

void
wifi_free_cache (void)
{
  if (!wifi_saved_mutex_inited)
    return;
  g_mutex_lock (&wifi_saved_mutex);
  if (wifi_saved_set != NULL)
    {
      g_hash_table_destroy (wifi_saved_set);
      wifi_saved_set = NULL;
    }
  wifi_saved_mtime = 0;
  g_mutex_unlock (&wifi_saved_mutex);
}

static const gchar *
wifi_signal_icon (gint signal)
{
  if (signal >= 75)
    return "network-wireless-signal-excellent-symbolic";
  if (signal >= 50)
    return "network-wireless-signal-good-symbolic";
  if (signal >= 25)
    return "network-wireless-signal-ok-symbolic";
  return "network-wireless-signal-weak-symbolic";
}

static void
wifi_style_card_row (GtkWidget *row,
                     GtkWidget *content)
{
  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  gtk_container_set_border_width (GTK_CONTAINER (content), QS_CONTENT_PAD);
}

/* 4th hbox child (check/lock badge): NULL when open+inactive. */
static GtkWidget *
wifi_badge_new (const WifiNet *net)
{
  GtkWidget *badge;

  if (net->active)
    badge = gtk_image_new_from_icon_name ("object-select-symbolic",
                                          GTK_ICON_SIZE_MENU);
  else if (net->secured)
    badge = gtk_image_new_from_icon_name ("system-lock-screen-symbolic",
                                          GTK_ICON_SIZE_MENU);
  else
    return NULL;
  gtk_image_set_pixel_size (GTK_IMAGE (badge), QS_META_ICON_PX);
  return badge;
}

static GtkWidget *
wifi_make_row (const WifiNet *net)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *icon = gtk_image_new_from_icon_name (net->active
                     ? "network-wireless-connected-symbolic"
                     : wifi_signal_icon (net->signal),
                     GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *name = gtk_label_new (net->ssid);
  GtkWidget *meta = gtk_label_new (NULL);
  GtkWidget *badge = wifi_badge_new (net);
  gchar     *text = g_strdup_printf ("%d%%", CLAMP (net->signal, 0, 100));

  gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
  ui_add_class (name, "qs-title");
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_text (GTK_LABEL (meta), text);
  g_free (text);
  ui_add_class (meta, "qs-sub");
  gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), meta, FALSE, FALSE, 0);
  if (badge != NULL)
    gtk_box_pack_start (GTK_BOX (hbox), badge, FALSE, FALSE, 0);
  wifi_style_card_row (row, hbox);
  gtk_container_add (GTK_CONTAINER (row), hbox);
  g_object_set_data_full (G_OBJECT (row), "qs-ssid",
                           g_strdup (net->ssid), g_free);
  g_object_set_data (G_OBJECT (row), "qs-secured",
                     GINT_TO_POINTER (net->secured));
  g_object_set_data (G_OBJECT (row), "qs-active",
                     GINT_TO_POINTER (net->active));
  return row;
}

/* Patches icon/meta/badge in place; reorders nothing (caller reorders). */
static void
wifi_patch_row (GtkWidget     *row,
                const WifiNet *net)
{
  GtkWidget *hbox = gtk_bin_get_child (GTK_BIN (row));
  GList     *kids;
  GtkWidget *icon = NULL;
  GtkWidget *meta = NULL;
  GtkWidget *badge_old = NULL;
  gint       labels_seen = 0;
  GList      *k;
  gboolean   old_secured;
  gboolean   old_active;

  if (hbox == NULL || !GTK_IS_BOX (hbox))
    return;
  kids = gtk_container_get_children (GTK_CONTAINER (hbox));
  for (k = kids; k != NULL; k = k->next)
    {
      if (GTK_IS_IMAGE (k->data))
        {
          if (icon == NULL)
            icon = GTK_WIDGET (k->data);
          else if (badge_old == NULL)
            badge_old = GTK_WIDGET (k->data);
        }
      else if (GTK_IS_LABEL (k->data))
        {
          labels_seen++;
          if (labels_seen == 1)
            {
              /* name label: SSID is the row key, already equal. */
            }
          else if (labels_seen == 2 && meta == NULL)
            meta = GTK_WIDGET (k->data);
        }
    }
  if (icon != NULL)
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                    net->active
                                    ? "network-wireless-connected-symbolic"
                                    : wifi_signal_icon (net->signal),
                                    GTK_ICON_SIZE_LARGE_TOOLBAR);
      gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
    }
  if (meta != NULL)
    {
      gchar *text = g_strdup_printf ("%d%%", CLAMP (net->signal, 0, 100));

      gtk_label_set_text (GTK_LABEL (meta), text);
      g_free (text);
    }
  old_secured = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row),
                                                    "qs-secured"));
  old_active = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row),
                                                   "qs-active"));
  if ((old_secured != net->secured) || (old_active != net->active))
    {
      if (badge_old != NULL)
        gtk_widget_destroy (badge_old);
      {
        GtkWidget *badge = wifi_badge_new (net);

        if (badge != NULL)
          {
            gtk_box_pack_start (GTK_BOX (hbox), badge, FALSE, FALSE, 0);
            gtk_widget_show (badge);
          }
      }
      g_object_set_data (G_OBJECT (row), "qs-secured",
                         GINT_TO_POINTER (net->secured));
      g_object_set_data (G_OBJECT (row), "qs-active",
                         GINT_TO_POINTER (net->active));
    }
  g_list_free (kids);
}

/* Diffs the list box by SSID: patches matching rows, appends new ones in
 * scan order, drops gone SSIDs (and stale placeholders). */
static void
wifi_fill_list (QuickSettings *qs,
                GList         *nets)
{
  GHashTable *wanted = NULL;   /* ssid -> WifiNet* */
  GHashTable *existing = NULL; /* ssid -> GtkListBoxRow* */
  GList      *rows;
  GList      *l;
  const gchar *active_ssid = NULL;
  GList      *nl;

  if (nets == NULL)
    {
      GList *kids = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
      GList *k;

      for (k = kids; k != NULL; k = k->next)
        gtk_widget_destroy (GTK_WIDGET (k->data));
      g_list_free (kids);
      gtk_container_add (GTK_CONTAINER (qs->wifi_list),
                         make_placeholder_row (qs->wifi_on
                                               ? "No networks found."
                                               : "Wi-Fi is off."));
      gtk_widget_show_all (qs->wifi_list);
      return;
    }

  wanted = g_hash_table_new (g_str_hash, g_str_equal);
  for (nl = nets; nl != NULL; nl = nl->next)
    {
      WifiNet *net = nl->data;

      g_hash_table_insert (wanted, net->ssid, net);
      if (net->active)
        active_ssid = net->ssid;
    }
  existing = g_hash_table_new (g_str_hash, g_str_equal);
  rows = gtk_container_get_children (GTK_CONTAINER (qs->wifi_list));
  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *ssid;

      if (!GTK_IS_LIST_BOX_ROW (l->data))
        {
          gtk_widget_destroy (GTK_WIDGET (l->data));
          continue;
        }
      ssid = g_object_get_data (G_OBJECT (l->data), "qs-ssid");
      if (ssid == NULL || g_hash_table_lookup (wanted, ssid) == NULL)
        gtk_widget_destroy (GTK_WIDGET (l->data));
      else
        g_hash_table_insert (existing, (gpointer) ssid, l->data);
    }
  g_list_free (rows);

  for (nl = nets; nl != NULL; nl = nl->next)
    {
      WifiNet   *net = nl->data;
      GtkWidget *row = g_hash_table_lookup (existing, net->ssid);

      if (row != NULL)
        {
          /* Patched in place; kept at its current position (GTK3
           * GtkListBox has no reorder API — order refreshes on the
           * next full change, state/focus never flickers). */
          wifi_patch_row (row, net);
        }
      else
        {
          row = wifi_make_row (net);
          gtk_container_add (GTK_CONTAINER (qs->wifi_list), row);
        }
    }
  g_hash_table_destroy (wanted);
  g_hash_table_destroy (existing);

  if (active_ssid != NULL)
    gtk_label_set_text (GTK_LABEL (qs->wifi_sub), active_ssid);
  else
    gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "Not connected");
  gtk_widget_show_all (qs->wifi_list);
}

/* Async Wi-Fi load: list + radio fetched together in the worker. */

typedef struct
{
  QuickSettings *qs;
  gchar         *list_out;
  gchar         *radio_out;
  gboolean       rescan;
} WifiLoad;

static gboolean wifi_load_idle (gpointer data);

static gpointer
wifi_load_thread (gpointer data)
{
  WifiLoad *w = data;
  gchar    *cmd;

  cmd = g_strdup_printf ("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY "
                         "device wifi list --rescan %s",
                         w->rescan ? "yes" : "no");
  w->list_out = run_command (cmd);
  g_free (cmd);
  w->radio_out = run_command ("nmcli radio wifi");
  g_idle_add (wifi_load_idle, w);
  return NULL;
}

static gboolean
wifi_load_idle (gpointer data)
{
  WifiLoad      *w = data;
  QuickSettings *qs = w->qs;
  GList         *nets;

  if (qs->dead)
    {
      g_free (w->list_out);
      g_free (w->radio_out);
      g_free (w);
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  qs->scanning_wifi = FALSE;
  gtk_widget_set_sensitive (qs->wifi_scan, TRUE);
  gtk_button_set_label (GTK_BUTTON (qs->wifi_scan), "Scan for networks");
  if (w->list_out == NULL)
    {
      gtk_label_set_text (GTK_LABEL (qs->wifi_status),
                          "Could not list networks.");
      g_free (w->list_out);
      g_free (w->radio_out);
      g_free (w);
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  nets = parse_wifi_list (w->list_out);
  qs->wifi_on = (w->radio_out != NULL
                 && g_str_has_prefix (g_strstrip (w->radio_out),
                                      "enabled"));
  qs->updating = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), qs->wifi_on);
  qs->updating = FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  wifi_fill_list (qs, nets);
  g_list_free_full (nets, wifi_net_free);
  g_free (w->list_out);
  g_free (w->radio_out);
  g_free (w);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static void
wifi_load_async (QuickSettings *qs,
                 gboolean       rescan)
{
  WifiLoad *w;

  if (qs->scanning_wifi)
    return;
  qs->scanning_wifi = TRUE;
  qs_ref (qs);
  gtk_widget_set_sensitive (qs->wifi_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->wifi_scan), "Scanning…");
  gtk_label_set_text (GTK_LABEL (qs->wifi_status),
                      rescan ? "Scanning for networks…" : "");
  w = g_new0 (WifiLoad, 1);
  w->qs = qs;
  w->rescan = rescan;
  g_thread_unref (g_thread_new ("qs-wifi", wifi_load_thread, w));
}

/* Async connect: saved-profile decision + nmcli run in the worker. */

typedef struct
{
  QuickSettings *qs;
  gchar         *ssid;
  gchar         *password; /* NULL when open/saved */
  gboolean       have_password;
  gchar         *output;
  gchar         *errout;
  gboolean       ok;
} WifiConnect;

static gboolean wifi_connect_idle (gpointer data);

static gpointer
wifi_connect_thread (gpointer data)
{
  WifiConnect *c = data;
  gchar       *qssid = g_shell_quote (c->ssid);
  gchar       *cmd = NULL;
  gboolean     saved = wifi_saved_query_sync (c->ssid);
  GError      *error = NULL;

  if (c->have_password)
    {
      if (saved)
        {
          cmd = g_strdup_printf ("nmcli connection up %s", qssid);
        }
      else
        {
          gchar *qpwd = g_shell_quote (c->password != NULL ? c->password
                                                           : "");

          cmd = g_strdup_printf ("nmcli device wifi connect %s password %s",
                                 qssid, qpwd);
          g_free (qpwd);
        }
    }
  else
    {
      if (saved)
        cmd = g_strdup_printf ("nmcli connection up %s", qssid);
      else
        cmd = g_strdup_printf ("nmcli device wifi connect %s", qssid);
    }
  g_free (qssid);
  c->ok = g_spawn_command_line_sync (cmd, &c->output, &c->errout,
                                     NULL, &error);
  if (error != NULL)
    g_clear_error (&error);
  g_free (cmd);
  wifi_saved_invalidate (); /* new profile may exist now */
  g_idle_add (wifi_connect_idle, c);
  return NULL;
}

static gboolean
wifi_connect_idle (gpointer data)
{
  WifiConnect *c = data;
  gchar       *msg;

  if (c->qs->dead)
    {
      g_free (c->ssid);
      g_free (c->password);
      g_free (c->output);
      g_free (c->errout);
      qs_unref (c->qs);
      g_free (c);
      return G_SOURCE_REMOVE;
    }
  if (c->ok)
    {
      msg = g_strdup_printf ("Connected to %s.", c->ssid);
    }
  else
    {
      gchar *first = g_strdup (c->errout != NULL ? c->errout : "");

      first[strcspn (first, "\n")] = '\0';
      if (*g_strstrip (first) != '\0')
        msg = first;
      else
        {
          g_free (first);
          msg = g_strdup ("Connection failed.");
        }
    }
  gtk_label_set_text (GTK_LABEL (c->qs->wifi_status), msg);
  g_free (msg);
  wifi_load_async (c->qs, FALSE);
  g_free (c->ssid);
  g_free (c->password);
  g_free (c->output);
  g_free (c->errout);
  qs_unref (c->qs);
  g_free (c);
  return G_SOURCE_REMOVE;
}

static void
wifi_connect_async (QuickSettings *qs,
                    const gchar   *ssid,
                    const gchar   *password)
{
  WifiConnect *c = g_new0 (WifiConnect, 1);

  c->qs = qs;
  c->ssid = g_strdup (ssid);
  if (password != NULL)
    {
      c->password = g_strdup (password);
      c->have_password = TRUE;
    }
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->wifi_status), "Connecting…");
  g_thread_unref (g_thread_new ("qs-wifi-connect", wifi_connect_thread, c));
}

static void
on_wifi_row_activated (GtkListBox    *box,
                       GtkListBoxRow *row,
                       gpointer       user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *ssid;
  gboolean       secured;

  (void) box;
  if (row == NULL || !gtk_widget_get_sensitive (GTK_WIDGET (row)))
    return;
  ssid = g_object_get_data (G_OBJECT (row), "qs-ssid");
  if (ssid == NULL)
    return;
  secured = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row),
                                                 "qs-secured"));
  /* Cache-only lookup (never spawns): stale/unknown SSIDs prompt, the safe
   * default; the worker re-decides saved vs new off the UI thread. */
  if (secured && !wifi_saved_lookup_cached (ssid))
    {
      GtkWidget *dlg = gtk_dialog_new_with_buttons ("Wi-Fi password",
                         GTK_WINDOW (qs->popup),
                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                         "_Cancel", GTK_RESPONSE_CANCEL,
                         "_Connect", GTK_RESPONSE_ACCEPT,
                         NULL);
      GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
      GtkWidget *prompt = gtk_label_new (ssid);
      GtkWidget *entry = gtk_entry_new ();

      gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
      gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
      gtk_box_pack_start (GTK_BOX (content), prompt, FALSE, FALSE, 6);
      gtk_box_pack_start (GTK_BOX (content), entry, FALSE, FALSE, 6);
      gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_ACCEPT);
      gtk_widget_show_all (dlg);
      if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_ACCEPT)
        wifi_connect_async (qs, ssid, gtk_entry_get_text (GTK_ENTRY (entry)));
      gtk_widget_destroy (dlg);
    }
  else
    {
      wifi_connect_async (qs, ssid, NULL);
    }
}

static gboolean
wifi_reload_timeout (gpointer data)
{
  QuickSettings *qs = data;

  if (!qs->dead)
    wifi_load_async (qs, FALSE);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static void
on_wifi_switch_changed (GObject    *obj,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  QuickSettings *qs = user_data;
  gboolean       on = gtk_switch_get_active (GTK_SWITCH (obj));

  (void) pspec;
  if (qs->updating)
    return;
  qs->wifi_on = on;
  set_tile_active (qs->wifi_tile, on);
  wifi_set_enabled (on);
  gtk_label_set_text (GTK_LABEL (qs->wifi_status),
                      on ? "Turning Wi-Fi on…" : "Turning Wi-Fi off…");
  qs_ref (qs);
  g_timeout_add_seconds (1, wifi_reload_timeout, qs);
}

static void
on_wifi_scan_clicked (GtkButton *button,
                      gpointer   user_data)
{
  (void) button;
  wifi_load_async ((QuickSettings *) user_data, TRUE);
}

/* Module-public (called by the qs-net facade). */

void
wifi_show_page (QuickSettings *qs)
{
  if (!wifi_available ())
    return;
  qs->updating = TRUE;
  qs->wifi_on = wifi_enabled_sync ();
  gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), qs->wifi_on);
  qs->updating = FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "wifi");
  wifi_load_async (qs, FALSE);
}

void
wifi_update_tile (QuickSettings *qs)
{
  qs->wifi_on = wifi_available () ? wifi_enabled_sync () : FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  gtk_widget_set_sensitive (qs->wifi_tile, wifi_available ());
  gtk_label_set_text (GTK_LABEL (qs->wifi_sub),
                      !wifi_available () ? "Unavailable"
                      : qs->wifi_on ? "On" : "Off");
}

/* Builds the Wi-Fi sub-page; back_out is left unwired for the facade. */
void
wifi_build_page (GtkWidget     *stack,
                 QuickSettings *qs,
                 GtkWidget    **back_out)
{
  GtkWidget *page = build_device_page ("Wi-Fi", "Scan for networks",
                                       &qs->wifi_switch, &qs->wifi_scan,
                                       &qs->wifi_list, &qs->wifi_status,
                                       back_out);

  wifi_saved_ensure ();
  gtk_stack_add_named (GTK_STACK (stack), page, "wifi");
  g_signal_connect (qs->wifi_switch, "notify::active",
                    G_CALLBACK (on_wifi_switch_changed), qs);
  g_signal_connect (qs->wifi_scan, "clicked",
                    G_CALLBACK (on_wifi_scan_clicked), qs);
  g_signal_connect (qs->wifi_list, "row-activated",
                    G_CALLBACK (on_wifi_row_activated), qs);
}

__attribute__((destructor)) static void
wifi_auto_cleanup (void)
{
  wifi_free_cache ();
}
