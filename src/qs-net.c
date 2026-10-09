#include "qs-net.h"
#include "qs-ui.h"
#include "qs-util.h"

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

/* Parses "IN-USE,SSID,SIGNAL,SECURITY" terse output. */
static GList *
parse_wifi_list (const gchar *output)
{
  GList  *nets = NULL;
  gchar **lines;
  gint    i;

  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar   **fields;
      WifiNet  *net;

      if (*lines[i] == '\0')
        continue;
      fields = split_terse_line (lines[i], 4);
      if (*fields[1] == '\0')
        {
          g_strfreev (fields);
          continue;
        }
      net = g_new0 (WifiNet, 1);
      net->ssid = g_strdup (fields[1]);
      net->signal = atoi (fields[2]);
      net->secured = !(g_strcmp0 (fields[3], "--") == 0 || *fields[3] == '\0');
      net->active = (g_strcmp0 (fields[0], "*") == 0);
      nets = g_list_append (nets, net);
      g_strfreev (fields);
    }
  g_strfreev (lines);
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

/* Fills paired/connected flags via "info MAC". */
static void
bt_fill_flags (BtDevice *dev)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s", dev->mac);
  gchar *output = run_command (cmd);

  if (output != NULL)
    {
      dev->paired = (strstr (output, "Paired: yes") != NULL);
      dev->connected = (strstr (output, "Connected: yes") != NULL);
      g_free (output);
    }
  g_free (cmd);
}

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

static void
wifi_fill_list (QuickSettings *qs,
                GList         *nets)
{
  GList *l;

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
      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *icon = gtk_image_new_from_icon_name (net->active
                         ? "network-wireless-connected-symbolic"
                         : wifi_signal_icon (net->signal),
                         GTK_ICON_SIZE_LARGE_TOOLBAR);
      GtkWidget *name = gtk_label_new (net->ssid);

      gtk_widget_set_halign (name, GTK_ALIGN_START);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
      if (net->active)
        {
          GtkWidget *check = gtk_image_new_from_icon_name ("emblem-ok-symbolic",
                                                           GTK_ICON_SIZE_MENU);

          gtk_box_pack_start (GTK_BOX (hbox), check, FALSE, FALSE, 0);
        }
      else if (net->secured)
        {
          GtkWidget *lock = gtk_image_new_from_icon_name ("system-lock-screen-symbolic",
                                                          GTK_ICON_SIZE_MENU);

          gtk_box_pack_start (GTK_BOX (hbox), lock, FALSE, FALSE, 0);
        }
      gtk_container_add (GTK_CONTAINER (row), hbox);
      g_object_set_data_full (G_OBJECT (row), "qs-ssid",
                              g_strdup (net->ssid), g_free);
      g_object_set_data (G_OBJECT (row), "qs-secured",
                         GINT_TO_POINTER (net->secured));
      gtk_container_add (GTK_CONTAINER (qs->wifi_list), row);
    }
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

  clear_list_box (qs->bt_list);
  if (devs == NULL)
    gtk_container_add (GTK_CONTAINER (qs->bt_list),
                       make_placeholder_row (empty_text));
  for (l = devs; l != NULL; l = l->next)
    {
      BtDevice  *dev = l->data;
      GtkWidget *row = gtk_list_box_row_new ();
      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *icon = gtk_image_new_from_icon_name (dev->connected
                         ? "bluetooth-active-symbolic"
                         : "bluetooth-symbolic",
                         GTK_ICON_SIZE_LARGE_TOOLBAR);
      GtkWidget *name = gtk_label_new (dev->name);
      GtkWidget *state;

      gtk_widget_set_halign (name, GTK_ALIGN_START);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      state = gtk_label_new (dev->connected ? "Connected"
                             : dev->paired ? "Paired" : "");
      gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
      gtk_box_pack_start (GTK_BOX (hbox), state, FALSE, FALSE, 0);
      gtk_container_add (GTK_CONTAINER (row), hbox);
      g_object_set_data_full (G_OBJECT (row), "qs-mac",
                              g_strdup (dev->mac), g_free);
      g_object_set_data_full (G_OBJECT (row), "qs-name",
                              g_strdup (dev->name), g_free);
      gtk_container_add (GTK_CONTAINER (qs->bt_list), row);
    }
  gtk_widget_show_all (qs->bt_list);
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
      bt_fill_list (qs, NULL, "No adapter.");
    }
  else if (!r->powered)
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "Bluetooth is off.");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      bt_fill_list (qs, NULL, "Turn Bluetooth on to see devices.");
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      bt_fill_list (qs, r->devs, "No devices found.");
    }

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
  gint           step; /* 0 pair, 1 trust, 2 connect */
} BtLink;

static const gchar *bt_link_verbs[] = { "pair", "trust", "connect" };
static const gchar *bt_link_doing[] = { "Pairing…", "Trusting…", "Connecting…" };

static void
bt_link_done (const gchar *output,
              const gchar *errout,
              gboolean     ok,
              gpointer     user_data)
{
  BtLink *l = user_data;
  gchar  *msg;
  gchar  *cmd;

  (void) output;
  (void) errout;
  if (l->qs->dead)
    {
      g_free (l->mac);
      g_free (l->name);
      qs_unref (l->qs);
      g_free (l);
      return;
    }
  if (!ok)
    {
      msg = g_strdup_printf ("Failed to %s %s.", bt_link_verbs[l->step],
                             l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
      g_free (l->mac);
      g_free (l->name);
      qs_unref (l->qs);
      g_free (l);
      return;
    }
  l->step++;
  if (l->step > 2)
    {
      msg = g_strdup_printf ("Connected to %s.", l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
      g_free (l->mac);
      g_free (l->name);
      qs_unref (l->qs);
      g_free (l);
      return;
    }
  gtk_label_set_text (GTK_LABEL (l->qs->bt_status), bt_link_doing[l->step]);
  cmd = g_strdup_printf ("bluetoothctl --timeout 20 %s %s",
                         bt_link_verbs[l->step], l->mac);
  run_command_async (cmd, bt_link_done, l);
  g_free (cmd);
}

static void
on_bt_row_activated (GtkListBox    *box,
                     GtkListBoxRow *row,
                     gpointer       user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *mac;
  const gchar   *name;
  BtLink        *l;
  gchar         *cmd;

  (void) box;
  if (row == NULL || !gtk_widget_get_sensitive (GTK_WIDGET (row)))
    return;
  mac = g_object_get_data (G_OBJECT (row), "qs-mac");
  name = g_object_get_data (G_OBJECT (row), "qs-name");
  if (mac == NULL)
    return;
  l = g_new0 (BtLink, 1);
  l->qs = qs;
  l->mac = g_strdup (mac);
  l->name = g_strdup (name != NULL ? name : mac);
  l->step = 0;
  gtk_label_set_text (GTK_LABEL (qs->bt_status), bt_link_doing[0]);
  cmd = g_strdup_printf ("bluetoothctl --timeout 20 %s %s",
                         bt_link_verbs[0], l->mac);
  qs_ref (qs);
  run_command_async (cmd, bt_link_done, l);
  g_free (cmd);
}

static void
show_bt_page (QuickSettings *qs)
{
  if (!command_exists ("bluetoothctl"))
    return;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "bt");
  if (qs->scanning_bt)
    return;
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
  /* Bluetooth state is cached: a live check may block without the daemon. */
  set_tile_active (qs->bt_tile, qs->bt_on);
  gtk_widget_set_sensitive (qs->bt_tile, command_exists ("bluetoothctl"));
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
  GtkWidget *tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *wifi_page;
  GtkWidget *bt_page;
  GtkWidget *wifi_back;
  GtkWidget *bt_back;

  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (conn_grid), tile_box, 0, 0, 2, 1);
  qs->wifi_tile = make_tile (tile_box, "network-wireless-symbolic", "Wi-Fi");
  g_signal_connect (qs->wifi_tile, "clicked",
                    G_CALLBACK (on_wifi_tile_clicked), qs);
  qs->bt_tile = make_tile (tile_box, "bluetooth-active-symbolic",
                            "Bluetooth");
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
  g_signal_connect (bt_back, "clicked",
                    G_CALLBACK (on_conn_back_clicked), qs);
}
