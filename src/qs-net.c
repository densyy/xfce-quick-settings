#include "qs-net.h"
#include "qs-ui.h"
#include "qs-util.h"
#include <string.h>

/* ---------------- Wi-Fi backend (nmcli) ---------------- */

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

static gboolean
wifi_available (void)
{
  return command_exists ("nmcli");
}

static gboolean
wifi_enabled (void)
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

/* Parses "IN-USE,SSID,SIGNAL,SECURITY" terse output.
 * Deduplicates by SSID (one row per SSID): scans often report several
 * BSSIDs with the same SSID, so keep only the strongest signal and mark
 * active if any BSSID is the current connection. */
static GList *
parse_wifi_list (const gchar *output)
{
  GList      *nets = NULL;
  GHashTable *seen;
  gchar     **lines;
  gint        i;

  if (output == NULL)
    return NULL;
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
      fields = split_terse_line (lines[i], 4);
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
          nets = g_list_append (nets, net);
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
  return nets;
}

/* Is there a saved connection profile for this SSID? */
static gboolean
wifi_profile_saved (const gchar *ssid)
{
  gchar    *output = run_command ("nmcli -t -f NAME,TYPE connection show");
  gchar   **lines;
  gboolean  saved = FALSE;
  gint      i;

  if (output == NULL)
    return FALSE;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL && !saved; i++)
    {
      gchar **fields = split_terse_line (lines[i], 2);

      if (g_strcmp0 (fields[0], ssid) == 0
          && g_strcmp0 (fields[1], "802-11-wireless") == 0)
        saved = TRUE;
      g_strfreev (fields);
    }
  g_strfreev (lines);
  g_free (output);
  return saved;
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
/* ---------------- Bluetooth backend (bluetoothctl) ---------------- */

typedef struct
{
  gchar    *mac;
  gchar    *name;
  gboolean  paired;
  gboolean  trusted;
  gboolean  connected;
} BtDevice;

static void
bt_device_free (gpointer data)
{
  BtDevice *dev = data;

  g_free (dev->mac);
  g_free (dev->name);
  g_free (dev);
}

/* Controller MAC or NULL (caller frees). Uses --timeout to avoid hangs. */
static gchar *
bt_get_controller (void)
{
  gchar   *output = run_command ("bluetoothctl --timeout 2 list");
  gchar  **lines;
  gchar   *mac = NULL;
  gint     i;

  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar **parts;

      if (!g_str_has_prefix (lines[i], "Controller "))
        continue;
      parts = g_strsplit (lines[i], " ", 4);
      if (parts[1] != NULL && *parts[1] != '\0')
        mac = g_strdup (parts[1]);
      g_strfreev (parts);
      break;
    }
  g_strfreev (lines);
  g_free (output);
  return mac;
}

static gboolean
bt_powered (const gchar *controller)
{
  gchar    *cmd;
  gchar    *output;
  gboolean  on = FALSE;

  if (controller == NULL)
    return FALSE;
  /* MAC contains only hex digits and colons: no quoting needed. */
  cmd = g_strdup_printf ("bluetoothctl --timeout 2 show %s", controller);
  output = run_command (cmd);
  if (output != NULL)
    {
      on = (strstr (output, "Powered: yes") != NULL);
      g_free (output);
    }
  g_free (cmd);
  return on;
}

static void
bt_set_powered (gboolean on)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 2 power %s",
                                on ? "on" : "off");

  spawn_command (cmd);
  g_free (cmd);
}

/* Fills paired/trusted/connected flags via "info MAC". */
static void
bt_fill_flags (BtDevice *dev)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s", dev->mac);
  gchar *output = run_command (cmd);

  if (output != NULL)
    {
      dev->paired = (strstr (output, "Paired: yes") != NULL);
      dev->trusted = (strstr (output, "Trusted: yes") != NULL);
      dev->connected = (strstr (output, "Connected: yes") != NULL);
      g_free (output);
    }
  g_free (cmd);
}

/* --- Bluetooth output classification ---
 * bluetoothctl exits 0 even on logical failures ("Failed to ...",
 * "not available", ...), so the text must be parsed. "Connection
 * successful" and "AlreadyExists" count as success even when wrapped in
 * a "Failed to ..." line (e.g. pair on an already-paired device). */
static gboolean
bt_str_contains_ci (const gchar *haystack,
                    const gchar *needle)
{
  gchar    *h;
  gchar    *n;
  gboolean  found = FALSE;

  if (haystack == NULL || needle == NULL)
    return FALSE;
  h = g_ascii_strdown (haystack, -1);
  n = g_ascii_strdown (needle, -1);
  if (strstr (h, n) != NULL)
    found = TRUE;
  g_free (h);
  g_free (n);
  return found;
}

static gboolean
bt_output_is_error (const gchar *output,
                    const gchar *errout)
{
  /* Success markers win over "Failed" wrappers. */
  if (bt_str_contains_ci (output, "successful")
      || bt_str_contains_ci (errout, "successful")
      || bt_str_contains_ci (output, "alreadyexists")
      || bt_str_contains_ci (errout, "alreadyexists"))
    return FALSE;
  if (bt_str_contains_ci (output, "failed")
      || bt_str_contains_ci (errout, "failed")
      || bt_str_contains_ci (output, "not available")
      || bt_str_contains_ci (errout, "not available")
      || bt_str_contains_ci (output, "no agent")
      || bt_str_contains_ci (errout, "no agent")
      || bt_str_contains_ci (output, "authenticationfailed")
      || bt_str_contains_ci (errout, "authenticationfailed")
      || bt_str_contains_ci (output, "authentication failed")
      || bt_str_contains_ci (errout, "authentication failed"))
    return TRUE;
  return FALSE;
}

static void
bt_info_text_flags (const gchar *output,
                    gboolean    *paired,
                    gboolean    *trusted,
                    gboolean    *connected)
{
  if (paired != NULL)
    *paired = (output != NULL && strstr (output, "Paired: yes") != NULL);
  if (trusted != NULL)
    *trusted = (output != NULL && strstr (output, "Trusted: yes") != NULL);
  if (connected != NULL)
    *connected = (output != NULL && strstr (output, "Connected: yes") != NULL);
}

/* --- Bluetooth in-memory cache (a few hours) ---
 * Opening the tab reuses the cache; only the Scan button forces a fresh
 * discovery + device listing. The cache is refreshed after every load
 * and patched in place after connect/disconnect. */
#define BT_CACHE_TTL_SECS (3 * 3600)

static GList  *bt_cache_devs = NULL;
static gchar  *bt_cache_controller = NULL;
static gboolean bt_cache_powered = FALSE;
static gint64 bt_cache_mtime = 0;

static BtDevice *
bt_device_copy (const BtDevice *src)
{
  BtDevice *dst = g_new0 (BtDevice, 1);

  dst->mac = g_strdup (src->mac);
  dst->name = g_strdup (src->name);
  dst->paired = src->paired;
  dst->trusted = src->trusted;
  dst->connected = src->connected;
  return dst;
}

static gboolean
bt_cache_valid (void)
{
  if (bt_cache_mtime == 0)
    return FALSE;
  return (g_get_monotonic_time () - bt_cache_mtime)
         < ((gint64) BT_CACHE_TTL_SECS * G_USEC_PER_SEC);
}

static void
bt_cache_store (const gchar *controller,
                gboolean     powered,
                GList       *devs)
{
  GList *l;

  g_list_free_full (bt_cache_devs, bt_device_free);
  bt_cache_devs = NULL;
  for (l = devs; l != NULL; l = l->next)
    bt_cache_devs = g_list_append (bt_cache_devs,
                                   bt_device_copy (l->data));
  g_free (bt_cache_controller);
  bt_cache_controller = g_strdup (controller);
  bt_cache_powered = powered;
  bt_cache_mtime = g_get_monotonic_time ();
}

static void
bt_cache_update (const gchar *mac,
                 gboolean     paired,
                 gboolean     trusted,
                 gboolean     connected,
                 gboolean     have_flags)
{
  GList *l;

  if (!have_flags || mac == NULL)
    return;
  for (l = bt_cache_devs; l != NULL; l = l->next)
    {
      BtDevice *dev = l->data;

      if (g_strcmp0 (dev->mac, mac) == 0)
        {
          dev->paired = paired;
          dev->trusted = trusted;
          dev->connected = connected;
          break;
        }
    }
}

/* Single in-flight connect/disconnect MAC (debounces row-selected +
 * row-activated firing for the same click). NULL when idle. */
static gchar *bt_busy_mac = NULL;

static gboolean
bt_is_busy (void)
{
  return bt_busy_mac != NULL;
}

static void
bt_set_busy (const gchar *mac)
{
  g_free (bt_busy_mac);
  bt_busy_mac = g_strdup (mac);
}

static void
bt_clear_busy (void)
{
  g_free (bt_busy_mac);
  bt_busy_mac = NULL;
}

/* While (re)filling the list, row-selected signals are programmatic
 * noise and must not start a connection. */
static gboolean bt_filling = FALSE;

static GList *
list_bt_devices (void)
{
  gchar  *output = run_command ("bluetoothctl --timeout 2 devices");
  gchar **lines;
  GList  *devs = NULL;
  gint    i;

  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar    **parts;
      BtDevice  *dev;

      if (!g_str_has_prefix (lines[i], "Device "))
        continue;
      parts = g_strsplit (lines[i], " ", 3);
      if (parts[1] == NULL || *parts[1] == '\0')
        {
          g_strfreev (parts);
          continue;
        }
      dev = g_new0 (BtDevice, 1);
      dev->mac = g_strdup (parts[1]);
      dev->name = g_strdup (parts[2] != NULL && *parts[2] != '\0'
                            ? parts[2] : parts[1]);
      bt_fill_flags (dev);
      devs = g_list_append (devs, dev);
      g_strfreev (parts);
    }
  g_strfreev (lines);
  g_free (output);
  return devs;
}
/* --- Wi-Fi page --- */

/* Card row: 20px icon + bold name + dim meta on the right. */
static void
style_card_row (GtkWidget *row,
                GtkWidget *content)
{
  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  gtk_container_set_border_width (GTK_CONTAINER (content), QS_CONTENT_PAD);
}

static void
wifi_fill_list (QuickSettings *qs,
                GList         *nets)
{
  GList *l;
  const gchar *active_ssid = NULL;

  clear_list_box (qs->wifi_list);
  if (nets == NULL)
    {
      gtk_container_add (GTK_CONTAINER (qs->wifi_list),
                         make_placeholder_row (wifi_enabled ()
                                               ? "No networks found."
                                               : "Wi-Fi is off."));
    }
  for (l = nets; l != NULL; l = l->next)
    {
      WifiNet   *net = l->data;
      GtkWidget *row = gtk_list_box_row_new ();
      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                     QS_INNER_GAP);
      GtkWidget *icon = gtk_image_new_from_icon_name (net->active
                         ? "network-wireless-connected-symbolic"
                         : wifi_signal_icon (net->signal),
                         GTK_ICON_SIZE_LARGE_TOOLBAR);
      GtkWidget *name = gtk_label_new (net->ssid);
      GtkWidget *meta;

      if (net->active)
        active_ssid = net->ssid;
      gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
      ui_add_class (name, "qs-title");
      gtk_widget_set_halign (name, GTK_ALIGN_START);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      meta = gtk_label_new (NULL);
      {
        gchar *text = g_strdup_printf ("%d%%", CLAMP (net->signal, 0, 100));

        gtk_label_set_text (GTK_LABEL (meta), text);
        g_free (text);
      }
      ui_add_class (meta, "qs-sub");
      gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), meta, FALSE, FALSE, 0);
      if (net->active)
        {
          GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic",
                                                           GTK_ICON_SIZE_MENU);

          gtk_image_set_pixel_size (GTK_IMAGE (check), QS_META_ICON_PX);
          gtk_box_pack_start (GTK_BOX (hbox), check, FALSE, FALSE, 0);
        }
      else if (net->secured)
        {
          GtkWidget *lock = gtk_image_new_from_icon_name ("system-lock-screen-symbolic",
                                                          GTK_ICON_SIZE_MENU);

          gtk_image_set_pixel_size (GTK_IMAGE (lock), QS_META_ICON_PX);
          gtk_box_pack_start (GTK_BOX (hbox), lock, FALSE, FALSE, 0);
        }
      style_card_row (row, hbox);
      gtk_container_add (GTK_CONTAINER (row), hbox);
      g_object_set_data_full (G_OBJECT (row), "qs-ssid",
                              g_strdup (net->ssid), g_free);
      g_object_set_data (G_OBJECT (row), "qs-secured",
                         GINT_TO_POINTER (net->secured));
      gtk_container_add (GTK_CONTAINER (qs->wifi_list), row);
    }
  if (active_ssid != NULL)
    gtk_label_set_text (GTK_LABEL (qs->wifi_sub), active_ssid);
  else if (nets != NULL)
    gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "Not connected");
  gtk_widget_show_all (qs->wifi_list);
}

static void
wifi_list_done (const gchar *output,
                const gchar *errout,
                gboolean     ok,
                gpointer     user_data)
{
  QuickSettings *qs = user_data;
  GList         *nets;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  qs->scanning_wifi = FALSE;
  gtk_widget_set_sensitive (qs->wifi_scan, TRUE);
  gtk_button_set_label (GTK_BUTTON (qs->wifi_scan), "Scan for networks");
  if (!ok)
    {
      gtk_label_set_text (GTK_LABEL (qs->wifi_status),
                          "Could not list networks.");
      qs_unref (qs);
      return;
    }
  nets = parse_wifi_list (output);
  qs->wifi_on = wifi_enabled ();
  qs->updating = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), qs->wifi_on);
  qs->updating = FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  wifi_fill_list (qs, nets);
  g_list_free_full (nets, wifi_net_free);
  qs_unref (qs);
}

static void
wifi_load_async (QuickSettings *qs,
                 gboolean       rescan)
{
  gchar *cmd;

  if (qs->scanning_wifi)
    return;
  qs->scanning_wifi = TRUE;
  qs_ref (qs);
  gtk_widget_set_sensitive (qs->wifi_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->wifi_scan), "Scanning…");
  gtk_label_set_text (GTK_LABEL (qs->wifi_status),
                      rescan ? "Scanning for networks…" : "");
  cmd = g_strdup_printf ("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY "
                         "device wifi list --rescan %s",
                         rescan ? "yes" : "no");
  run_command_async (cmd, wifi_list_done, qs);
  g_free (cmd);
}

typedef struct
{
  QuickSettings *qs;
  gchar         *ssid;
  gchar         *password; /* NULL when open/saved */
} WifiConnect;

static void
wifi_connect_done (const gchar *output,
                   const gchar *errout,
                   gboolean     ok,
                   gpointer     user_data)
{
  WifiConnect *c = user_data;
  gchar       *msg;

  (void) output;
  if (c->qs->dead)
    {
      g_free (c->ssid);
      g_free (c->password);
      qs_unref (c->qs);
      g_free (c);
      return;
    }
  if (ok)
    {
      msg = g_strdup_printf ("Connected to %s.", c->ssid);
    }
  else
    {
      gchar *first = g_strdup (errout != NULL ? errout : "");

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
  qs_unref (c->qs);
  g_free (c);
}

static void
wifi_connect_async (QuickSettings *qs,
                    const gchar   *ssid,
                    const gchar   *password)
{
  WifiConnect *c = g_new0 (WifiConnect, 1);
  gchar       *qssid = g_shell_quote (ssid);
  gchar       *cmd;

  c->qs = qs;
  c->ssid = g_strdup (ssid);
  if (password != NULL)
    {
      gchar *qpwd = g_shell_quote (password);

      c->password = g_strdup (password);
      if (wifi_profile_saved (ssid))
        cmd = g_strdup_printf ("nmcli connection up %s", qssid);
      else
        cmd = g_strdup_printf ("nmcli device wifi connect %s password %s",
                               qssid, qpwd);
      g_free (qpwd);
    }
  else
    {
      if (wifi_profile_saved (ssid))
        cmd = g_strdup_printf ("nmcli connection up %s", qssid);
      else
        cmd = g_strdup_printf ("nmcli device wifi connect %s", qssid);
    }
  g_free (qssid);
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->wifi_status), "Connecting…");
  run_command_async (cmd, wifi_connect_done, c);
  g_free (cmd);
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
  secured = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "qs-secured"));
  if (secured && !wifi_profile_saved (ssid))
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

static void
show_wifi_page (QuickSettings *qs)
{
  if (!wifi_available ())
    return;
  qs->updating = TRUE;
  qs->wifi_on = wifi_enabled ();
  gtk_switch_set_active (GTK_SWITCH (qs->wifi_switch), qs->wifi_on);
  qs->updating = FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "wifi");
  wifi_load_async (qs, FALSE);
}
/* --- Bluetooth page --- */

static void
bt_fill_list (QuickSettings *qs,
              GList         *devs,
               const gchar   *empty_text)
{
  GList *l;
  const gchar *connected_name = NULL;

  bt_filling = TRUE;
  clear_list_box (qs->bt_list);
  if (devs == NULL)
    gtk_container_add (GTK_CONTAINER (qs->bt_list),
                       make_placeholder_row (empty_text));
  for (l = devs; l != NULL; l = l->next)
    {
      BtDevice  *dev = l->data;
      GtkWidget *row = gtk_list_box_row_new ();
      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                     QS_INNER_GAP);
      GtkWidget *icon = gtk_image_new_from_icon_name (dev->connected
                         ? "bluetooth-active-symbolic"
                         : "bluetooth-symbolic",
                         GTK_ICON_SIZE_LARGE_TOOLBAR);
      GtkWidget *name = gtk_label_new (dev->name);
      GtkWidget *state;

      if (dev->connected)
        connected_name = dev->name;
      gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
      ui_add_class (name, "qs-title");
      gtk_widget_set_halign (name, GTK_ALIGN_START);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      state = gtk_label_new (dev->connected ? "Connected"
                             : dev->paired ? "Paired" : "");
      ui_add_class (state, "qs-sub");
      gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), state, FALSE, FALSE, 0);
      style_card_row (row, hbox);
      gtk_container_add (GTK_CONTAINER (row), hbox);
      g_object_set_data_full (G_OBJECT (row), "qs-mac",
                              g_strdup (dev->mac), g_free);
      g_object_set_data_full (G_OBJECT (row), "qs-name",
                              g_strdup (dev->name), g_free);
      g_object_set_data (G_OBJECT (row), "qs-connected",
                         GINT_TO_POINTER (dev->connected));
      g_object_set_data (G_OBJECT (row), "qs-paired",
                         GINT_TO_POINTER (dev->paired));
      gtk_container_add (GTK_CONTAINER (qs->bt_list), row);
    }
  if (connected_name != NULL)
    gtk_label_set_text (GTK_LABEL (qs->bt_sub), connected_name);
  else if (devs != NULL)
    gtk_label_set_text (GTK_LABEL (qs->bt_sub), "On");
  gtk_widget_show_all (qs->bt_list);
  bt_filling = FALSE;
}

/* Updates the row for MAC to "Connected"/"Paired"/"" in place, without
 * requiring a manual Scan. Also refreshes the subtitle.
 * Returns TRUE when the row was found and patched. */
static gboolean
bt_update_row (QuickSettings *qs,
               const gchar   *mac,
               gboolean       connected,
               gboolean       paired)
{
  GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->bt_list));
  GList *l;
  gboolean found = FALSE;

  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *m;
      GtkWidget   *hbox;
      GList       *kids;
      GList       *k;
      GtkWidget   *icon = NULL;
      GtkWidget   *state = NULL;
      gint         labels_seen = 0;

      if (!GTK_IS_LIST_BOX_ROW (l->data))
        continue;
      m = g_object_get_data (G_OBJECT (l->data), "qs-mac");
      if (g_strcmp0 (m, mac) != 0)
        continue;
      hbox = gtk_bin_get_child (GTK_BIN (l->data));
      if (hbox == NULL || !GTK_IS_BOX (hbox))
        break;
      kids = gtk_container_get_children (GTK_CONTAINER (hbox));
      for (k = kids; k != NULL; k = k->next)
        {
          if (GTK_IS_IMAGE (k->data) && icon == NULL)
            icon = GTK_WIDGET (k->data);
          else if (GTK_IS_LABEL (k->data))
            {
              /* hbox packs icon, name, state: second label is the state. */
              labels_seen++;
              if (labels_seen == 2)
                state = GTK_WIDGET (k->data);
            }
        }
      if (state != NULL)
        gtk_label_set_text (GTK_LABEL (state),
                            connected ? "Connected"
                            : paired ? "Paired" : "");
      if (icon != NULL)
        gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                      connected
                                      ? "bluetooth-active-symbolic"
                                      : "bluetooth-symbolic",
                                      GTK_ICON_SIZE_LARGE_TOOLBAR);
      g_list_free (kids);
      g_object_set_data (G_OBJECT (l->data), "qs-connected",
                         GINT_TO_POINTER (connected));
      g_object_set_data (G_OBJECT (l->data), "qs-paired",
                         GINT_TO_POINTER (paired));
      if (connected)
        {
          const gchar *nm = g_object_get_data (G_OBJECT (l->data), "qs-name");

          if (nm != NULL)
            gtk_label_set_text (GTK_LABEL (qs->bt_sub), nm);
        }
      else
        {
          const gchar *nm = g_object_get_data (G_OBJECT (l->data), "qs-name");
          const gchar *sub = gtk_label_get_text (GTK_LABEL (qs->bt_sub));

          if (nm != NULL && sub != NULL && g_strcmp0 (nm, sub) == 0)
            gtk_label_set_text (GTK_LABEL (qs->bt_sub), "On");
        }
      found = TRUE;
      break;
    }
  g_list_free (rows);
  return found;
}

typedef struct
{
  QuickSettings *qs;
  GList         *devs;
  gchar         *controller; /* owned, transfer to qs */
  gboolean       powered;
} BtResult;

static gboolean
bt_load_idle (gpointer data);

static gpointer
bt_load_thread (gpointer data)
{
  BtResult *r = data;

  r->controller = bt_get_controller ();
  if (r->controller != NULL)
    r->powered = bt_powered (r->controller);
  if (r->powered)
    r->devs = list_bt_devices ();
  g_idle_add (bt_load_idle, r);
  return NULL;
}

static gboolean
bt_load_idle (gpointer data)
{
  BtResult      *r = data;
  QuickSettings *qs = r->qs;

  if (qs->dead)
    {
      g_list_free_full (r->devs, bt_device_free);
      g_free (r->controller);
      g_free (r);
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }

  g_free (qs->bt_controller);
  qs->bt_controller = r->controller;
  r->controller = NULL;
  qs->bt_on = (qs->bt_controller != NULL && r->powered);
  qs->updating = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), qs->bt_on);
  qs->updating = FALSE;
  set_tile_active (qs->bt_tile, qs->bt_on);

  if (qs->bt_controller == NULL)
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status),
                          command_exists ("bluetoothctl")
                          ? "No Bluetooth adapter found."
                          : "Install bluez-utils.");
      gtk_widget_set_sensitive (qs->bt_switch, FALSE);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), "No adapter");
      bt_fill_list (qs, NULL, "No adapter.");
    }
  else if (!r->powered)
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "Bluetooth is off.");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), "Off");
      bt_fill_list (qs, NULL, "Turn Bluetooth on to see devices.");
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      bt_fill_list (qs, r->devs, "No devices found.");
    }

  bt_cache_store (qs->bt_controller, r->powered, r->devs);
  qs->scanning_bt = FALSE;
  gtk_widget_set_sensitive (qs->bt_scan, TRUE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scan for devices");

  g_list_free_full (r->devs, bt_device_free);
  g_free (r->controller);
  g_free (r);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static void
bt_start_load_thread (QuickSettings *qs)
{
  BtResult *r = g_new0 (BtResult, 1);

  r->qs = qs;
  qs_ref (qs);
  g_thread_unref (g_thread_new ("qs-bt", bt_load_thread, r));
}

static void
bt_scan_done (const gchar *output,
              const gchar *errout,
              gboolean     ok,
              gpointer     user_data)
{
  QuickSettings *qs = user_data;

  (void) output;
  (void) errout;
  (void) ok;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Reading devices…");
  bt_start_load_thread (qs);
  qs_unref (qs);
}

static void
on_bt_scan_clicked (GtkButton *button,
                    gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  if (qs->scanning_bt)
    return;
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scanning…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Scanning for devices…");
  /* Discovery stops by itself when bluetoothctl exits after the timeout. */
  qs_ref (qs);
  run_command_async ("bluetoothctl --timeout 10 scan on", bt_scan_done, qs);
}

static gboolean
bt_reload_timeout (gpointer data)
{
  QuickSettings *qs = data;

  if (qs->dead)
    {
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  if (qs->scanning_bt)
    {
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  bt_start_load_thread (qs);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static void
on_bt_switch_changed (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  QuickSettings *qs = user_data;
  gboolean       on = gtk_switch_get_active (GTK_SWITCH (obj));

  (void) pspec;
  if (qs->updating)
    return;
  qs->bt_on = on;
  set_tile_active (qs->bt_tile, on);
  bt_set_powered (on);
  gtk_label_set_text (GTK_LABEL (qs->bt_status),
                      on ? "Turning Bluetooth on…"
                         : "Turning Bluetooth off…");
  qs_ref (qs);
  g_timeout_add_seconds (1, bt_reload_timeout, qs);
}

typedef struct
{
  QuickSettings *qs;
  gchar         *mac;
  gchar         *name;
  gchar        **verbs; /* remaining verbs, NULL-terminated (owned) */
  gint           idx;   /* next verb to run */
  gboolean       disconnecting;
  gint           verify_tries; /* remaining `info` re-reads in verify stage */
} BtLink;

/* Verify stage: `disconnect`/`connect` return before BlueZ flips the
 * Connected flag, so a single `info` read right after the verb often
 * still shows the old state. Retrying the `info` read a few times on
 * the main loop closes that race; without it the row keeps the stale
 * "Connected" label until the next manual Scan. */
#define BT_VERIFY_MAX_TRIES 5
#define BT_VERIFY_DELAY_MS 800

static const gchar *
bt_doing_label (const gchar *verb)
{
  if (g_strcmp0 (verb, "pair") == 0)
    return "Pairing…";
  if (g_strcmp0 (verb, "trust") == 0)
    return "Trusting…";
  if (g_strcmp0 (verb, "disconnect") == 0)
    return "Disconnecting…";
  return "Connecting…";
}

static void
bt_link_free (BtLink *l)
{
  if (l == NULL)
    return;
  g_free (l->mac);
  g_free (l->name);
  g_strfreev (l->verbs);
  qs_unref (l->qs);
  g_free (l);
}

static void
bt_link_finish (BtLink *l)
{
  bt_clear_busy ();
  bt_link_free (l);
}

/* First non-empty line of output/errout for status details (owned or NULL). */
static gchar *
bt_first_detail (const gchar *output,
                 const gchar *errout)
{
  const gchar *src = (output != NULL && *output != '\0') ? output : errout;
  gchar       *copy;
  gchar       *nl;
  gchar       *stripped;

  if (src == NULL || *src == '\0')
    return NULL;
  copy = g_strdup (src);
  nl = strchr (copy, '\n');
  if (nl != NULL)
    *nl = '\0';
  stripped = g_strstrip (copy);
  if (*stripped == '\0')
    {
      g_free (copy);
      return NULL;
    }
  memmove (copy, stripped, strlen (stripped) + 1);
  return copy;
}

static void
bt_link_fail (BtLink      *l,
              const gchar *verb,
              const gchar *detail)
{
  gchar *msg;

  if (!l->qs->dead)
    {
      if (detail != NULL && *detail != '\0')
        msg = g_strdup_printf ("Failed to %s %s: %s", verb, l->name, detail);
      else
        msg = g_strdup_printf ("Failed to %s %s.", verb, l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
    }
  bt_link_finish (l);
}

static void bt_link_run_next (BtLink *l);
static void bt_link_verify_done (const gchar *output,
                                 const gchar *errout,
                                 gboolean     ok,
                                 gpointer     user_data);

/* Re-reads `info` after a short delay while the verify stage is still
 * inconclusive. Runs on the main loop; the BtLink (and its qs ref and
 * bt_busy_mac) stays alive until the final verdict, so the debounce
 * keeps swallowing duplicate clicks but never the refresh itself. */
static gboolean
bt_verify_timeout (gpointer data)
{
  BtLink *l = data;
  gchar  *cmd;

  if (l->qs->dead)
    {
      bt_link_finish (l);
      return G_SOURCE_REMOVE;
    }
  cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s", l->mac);
  run_command_async (cmd, bt_link_verify_done, l);
  g_free (cmd);
  return G_SOURCE_REMOVE;
}

static void
bt_link_verify_done (const gchar *output,
                     const gchar *errout,
                     gboolean     ok,
                     gpointer     user_data)
{
  BtLink  *l = user_data;
  gboolean paired = FALSE;
  gboolean trusted = FALSE;
  gboolean connected = FALSE;
  gchar   *msg;

  (void) errout;
  (void) ok;
  if (l->qs->dead)
    {
      bt_link_finish (l);
      return;
    }
  bt_info_text_flags (output, &paired, &trusted, &connected);
  if (l->disconnecting)
    {
      if (!connected)
        {
          msg = g_strdup_printf ("Disconnected from %s.", l->name);
          gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
          g_free (msg);
          /* Cache first so the fallback refill shows the new state;
           * bt_update_row patches the row in place on the main thread
           * (this callback already runs via g_idle_add) and releases
           * bt_busy_mac through bt_link_finish below. */
          bt_cache_update (l->mac, paired, trusted, FALSE, TRUE);
          if (!bt_update_row (l->qs, l->mac, FALSE, paired))
            bt_fill_list (l->qs, bt_cache_devs, "No devices found.");
          bt_link_finish (l);
        }
      else if (l->verify_tries > 1)
        {
          l->verify_tries--;
          gtk_label_set_text (GTK_LABEL (l->qs->bt_status), "Verifying…");
          g_timeout_add (BT_VERIFY_DELAY_MS, bt_verify_timeout, l);
        }
      else
        {
          bt_link_fail (l, "disconnect", NULL);
        }
      return;
    }
  if (connected)
    {
      msg = g_strdup_printf ("Connected to %s.", l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
      bt_cache_update (l->mac, paired, trusted, TRUE, TRUE);
      if (!bt_update_row (l->qs, l->mac, TRUE, paired))
        bt_fill_list (l->qs, bt_cache_devs, "No devices found.");
      bt_link_finish (l);
    }
  else if (l->verify_tries > 1)
    {
      l->verify_tries--;
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), "Verifying…");
      g_timeout_add (BT_VERIFY_DELAY_MS, bt_verify_timeout, l);
    }
  else
    {
      gchar *detail = bt_first_detail (output, errout);

      bt_link_fail (l, "connect", detail);
      g_free (detail);
    }
}

static void
bt_link_done (const gchar *output,
              const gchar *errout,
              gboolean     ok,
              gpointer     user_data)
{
  BtLink      *l = user_data;
  const gchar *verb;

  if (l->qs->dead)
    {
      bt_link_finish (l);
      return;
    }
  verb = (l->verbs != NULL) ? l->verbs[l->idx] : "connect";
  if (verb == NULL)
    verb = l->disconnecting ? "disconnect" : "connect";
  if (!ok || bt_output_is_error (output, errout))
    {
      gchar *detail = bt_first_detail (output, errout);

      bt_link_fail (l, verb, detail);
      g_free (detail);
      return;
    }
  l->idx++;
  bt_link_run_next (l);
}

static void
bt_link_run_next (BtLink *l)
{
  gchar *cmd;

  if (l->qs->dead)
    {
      bt_link_finish (l);
      return;
    }
  if (l->verbs == NULL || l->verbs[l->idx] == NULL)
    {
      /* All verbs done: only trust `info` saying Connected yes/no. */
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), "Verifying…");
      l->verify_tries = BT_VERIFY_MAX_TRIES;
      cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s", l->mac);
      run_command_async (cmd, bt_link_verify_done, l);
      g_free (cmd);
      return;
    }
  gtk_label_set_text (GTK_LABEL (l->qs->bt_status),
                      bt_doing_label (l->verbs[l->idx]));
  cmd = g_strdup_printf ("bluetoothctl --timeout 20 %s %s",
                         l->verbs[l->idx], l->mac);
  run_command_async (cmd, bt_link_done, l);
  g_free (cmd);
}

/* Decides connect vs disconnect from a fresh `info` snapshot. Connected
 * devices disconnect; others connect, skipping pair/trust when `info`
 * already reports Paired/Trusted yes. */
static void
bt_toggle_info_done (const gchar *output,
                     const gchar *errout,
                     gboolean     ok,
                     gpointer     user_data)
{
  BtLink  *l = user_data;
  gboolean paired = FALSE;
  gboolean trusted = FALSE;
  gboolean connected = FALSE;

  (void) errout;
  (void) ok;
  if (l->qs->dead)
    {
      bt_link_finish (l);
      return;
    }
  if (output == NULL)
    {
      bt_link_fail (l, "read", "could not query device info");
      return;
    }
  bt_info_text_flags (output, &paired, &trusted, &connected);
  if (connected)
    {
      l->disconnecting = TRUE;
      l->verbs = g_new0 (gchar *, 2);
      l->verbs[0] = g_strdup ("disconnect");
      l->idx = 0;
      bt_link_run_next (l);
    }
  else
    {
      GPtrArray *arr = g_ptr_array_new ();

      if (!paired)
        g_ptr_array_add (arr, g_strdup ("pair"));
      if (!trusted)
        g_ptr_array_add (arr, g_strdup ("trust"));
      g_ptr_array_add (arr, g_strdup ("connect"));
      g_ptr_array_add (arr, NULL);
      l->disconnecting = FALSE;
      l->verbs = (gchar **) g_ptr_array_free (arr, FALSE);
      l->idx = 0;
      bt_link_run_next (l);
    }
}

static void
bt_toggle_device (QuickSettings *qs,
                  GtkListBoxRow *row)
{
  const gchar *mac;
  const gchar *name;
  BtLink      *l;
  gchar       *cmd;

  if (row == NULL || !gtk_widget_get_sensitive (GTK_WIDGET (row)))
    return;
  if (bt_filling || bt_is_busy () || qs->dead)
    return;
  mac = g_object_get_data (G_OBJECT (row), "qs-mac");
  name = g_object_get_data (G_OBJECT (row), "qs-name");
  if (mac == NULL)
    return;
  l = g_new0 (BtLink, 1);
  l->qs = qs;
  l->mac = g_strdup (mac);
  l->name = g_strdup (name != NULL ? name : mac);
  l->verbs = NULL;
  l->idx = 0;
  l->disconnecting = FALSE;
  bt_set_busy (mac);
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Reading device…");
  cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s", l->mac);
  run_command_async (cmd, bt_toggle_info_done, l);
  g_free (cmd);
}

static void
on_bt_row_activated (GtkListBox    *box,
                     GtkListBoxRow *row,
                     gpointer       user_data)
{
  (void) box;
  bt_toggle_device ((QuickSettings *) user_data, row);
}

static void
on_bt_row_selected (GtkListBox    *box,
                    GtkListBoxRow *row,
                    gpointer       user_data)
{
  (void) box;
  /* Single click starts the toggle; a second activation on a connected
   * device therefore disconnects. Guarded by bt_filling/bt_busy. */
  bt_toggle_device ((QuickSettings *) user_data, row);
}

static void
show_bt_page (QuickSettings *qs)
{
  if (!command_exists ("bluetoothctl"))
    return;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "bt");
  if (qs->scanning_bt)
    return;
  if (bt_cache_valid ())
    {
      /* Reuse the in-memory cache; only Scan fetches new devices. */
      g_free (qs->bt_controller);
      qs->bt_controller = g_strdup (bt_cache_controller);
      qs->bt_on = (qs->bt_controller != NULL && bt_cache_powered);
      qs->updating = TRUE;
      gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), qs->bt_on);
      qs->updating = FALSE;
      set_tile_active (qs->bt_tile, qs->bt_on);
      if (qs->bt_controller == NULL)
        {
          gtk_label_set_text (GTK_LABEL (qs->bt_status),
                              command_exists ("bluetoothctl")
                              ? "No Bluetooth adapter found."
                              : "Install bluez-utils.");
          gtk_widget_set_sensitive (qs->bt_switch, FALSE);
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "No adapter");
          bt_fill_list (qs, NULL, "No adapter.");
        }
      else if (!bt_cache_powered)
        {
          gtk_label_set_text (GTK_LABEL (qs->bt_status), "Bluetooth is off.");
          gtk_widget_set_sensitive (qs->bt_switch, TRUE);
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "Off");
          bt_fill_list (qs, NULL, "Turn Bluetooth on to see devices.");
        }
      else
        {
          gtk_label_set_text (GTK_LABEL (qs->bt_status), "");
          gtk_widget_set_sensitive (qs->bt_switch, TRUE);
          bt_fill_list (qs, bt_cache_devs, "No devices found.");
        }
      gtk_widget_set_sensitive (qs->bt_scan, TRUE);
      gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scan for devices");
      return;
    }
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Loading…");
  bt_start_load_thread (qs);
}

/* --- tiles --- */

void
net_update_tiles (QuickSettings *qs)
{
  qs->wifi_on = wifi_available () ? wifi_enabled () : FALSE;
  set_tile_active (qs->wifi_tile, qs->wifi_on);
  gtk_widget_set_sensitive (qs->wifi_tile, wifi_available ());
  gtk_label_set_text (GTK_LABEL (qs->wifi_sub),
                      !wifi_available () ? "Unavailable"
                      : qs->wifi_on ? "On" : "Off");
  /* Bluetooth state is live-probed async on popup open (see below):
   * a sync check here may block without the daemon. Show the last
   * known state now; the probe corrects it moments later. */
  set_tile_active (qs->bt_tile, qs->bt_on);
  gtk_widget_set_sensitive (qs->bt_tile, command_exists ("bluetoothctl"));
}

/* --- Bluetooth tile live probe (non-blocking) ---
 * Refreshes the tile (+ switch) from the real controller state every
 * time the popup opens, without stalling it when bluetoothd is down. */

typedef struct
{
  QuickSettings *qs;
  gchar         *controller; /* owned MAC or NULL */
  gboolean       powered;
} BtTileProbe;

static gboolean
bt_tile_probe_idle (gpointer data)
{
  BtTileProbe   *p = data;
  QuickSettings *qs = p->qs;

  qs->bt_tile_probe = FALSE;
  /* A full page load owns the widgets while it runs; it updates the
   * tile itself when done. */
  if (!qs->dead && !qs->scanning_bt)
    {
      g_free (qs->bt_controller);
      qs->bt_controller = p->controller;
      p->controller = NULL;
      qs->bt_on = (qs->bt_controller != NULL && p->powered);
      qs->updating = TRUE;
      gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), qs->bt_on);
      qs->updating = FALSE;
      set_tile_active (qs->bt_tile, qs->bt_on);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub),
                          qs->bt_controller == NULL ? "No adapter"
                          : qs->bt_on ? "On" : "Off");
    }
  g_free (p->controller);
  g_free (p);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static gpointer
bt_tile_probe_thread (gpointer data)
{
  BtTileProbe *p = data;

  p->controller = bt_get_controller ();
  if (p->controller != NULL)
    p->powered = bt_powered (p->controller);
  g_idle_add (bt_tile_probe_idle, p);
  return NULL;
}

void
net_refresh_bt_tile (QuickSettings *qs)
{
  BtTileProbe *p;

  if (!command_exists ("bluetoothctl"))
    return;
  if (qs->bt_tile_probe || qs->scanning_bt)
    return;
  qs->bt_tile_probe = TRUE;
  p = g_new0 (BtTileProbe, 1);
  p->qs = qs;
  qs_ref (qs);
  g_thread_unref (g_thread_new ("qs-bt-tile", bt_tile_probe_thread, p));
}

static void
on_wifi_tile_clicked (GtkButton *button,
                      gpointer   user_data)
{
  (void) button;
  show_wifi_page ((QuickSettings *) user_data);
}

static void
on_bt_tile_clicked (GtkButton *button,
                    gpointer   user_data)
{
  (void) button;
  show_bt_page ((QuickSettings *) user_data);
}

static void
on_conn_back_clicked (GtkButton *button,
                      gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  net_update_tiles (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
}

/* ---------------- section: build ---------------- */

void
net_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *conn_grid = make_group (box, "Connection");
  GtkWidget *tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                     QS_INNER_GAP);
  GtkWidget *wifi_page;
  GtkWidget *bt_page;
  GtkWidget *wifi_back;
  GtkWidget *bt_back;

  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (conn_grid), tile_box, 0, 0, 2, 1);
  qs->wifi_tile = make_nav_tile (tile_box, "network-wireless-symbolic",
                                 "Wi-Fi", &qs->wifi_sub);
  gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "…");
  g_signal_connect (qs->wifi_tile, "clicked",
                    G_CALLBACK (on_wifi_tile_clicked), qs);
  qs->bt_tile = make_nav_tile (tile_box, "bluetooth-active-symbolic",
                                "Bluetooth", &qs->bt_sub);
  gtk_label_set_text (GTK_LABEL (qs->bt_sub), "…");
  g_signal_connect (qs->bt_tile, "clicked",
                    G_CALLBACK (on_bt_tile_clicked), qs);

  /* Sub-pages. */
  wifi_page = build_device_page ("Wi-Fi", "Scan for networks",
                                 &qs->wifi_switch, &qs->wifi_scan,
                                 &qs->wifi_list, &qs->wifi_status, &wifi_back);
  gtk_stack_add_named (GTK_STACK (stack), wifi_page, "wifi");
  bt_page = build_device_page ("Bluetooth", "Scan for devices",
                               &qs->bt_switch, &qs->bt_scan,
                               &qs->bt_list, &qs->bt_status, &bt_back);
  gtk_stack_add_named (GTK_STACK (stack), bt_page, "bt");

  g_signal_connect (qs->wifi_switch, "notify::active",
                    G_CALLBACK (on_wifi_switch_changed), qs);
  g_signal_connect (qs->wifi_scan, "clicked",
                    G_CALLBACK (on_wifi_scan_clicked), qs);
  g_signal_connect (qs->wifi_list, "row-activated",
                    G_CALLBACK (on_wifi_row_activated), qs);
  g_signal_connect (wifi_back, "clicked",
                    G_CALLBACK (on_conn_back_clicked), qs);
  g_signal_connect (qs->bt_switch, "notify::active",
                    G_CALLBACK (on_bt_switch_changed), qs);
  g_signal_connect (qs->bt_scan, "clicked",
                    G_CALLBACK (on_bt_scan_clicked), qs);
  g_signal_connect (qs->bt_list, "row-activated",
                    G_CALLBACK (on_bt_row_activated), qs);
  g_signal_connect (qs->bt_list, "row-selected",
                    G_CALLBACK (on_bt_row_selected), qs);
  g_signal_connect (bt_back, "clicked",
                    G_CALLBACK (on_conn_back_clicked), qs);
}
