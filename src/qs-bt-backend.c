#include "qs-bt-backend.h"
#include "qs-util.h"

#include <gio/gio.h>
#include <string.h>

void
bt_device_free (gpointer data)
{
  BtDevice *dev = data;

  g_free (dev->mac);
  g_free (dev->name);
  g_free (dev);
}

/* Controller MAC or NULL (caller frees). Uses --timeout to avoid hangs. */
gchar *
bt_get_controller (void)
{
  gchar  *output = run_command ("bluetoothctl --timeout 1 list");
  gchar **lines;
  gchar  *mac = NULL;
  gint    i;

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

gboolean
bt_powered (const gchar *controller)
{
  gchar    *cmd;
  gchar    *output;
  gboolean  on = FALSE;

  if (controller == NULL)
    return FALSE;
  {
    gchar *controller_q = g_shell_quote (controller);

    cmd = g_strdup_printf ("bluetoothctl --timeout 1 show %s", controller_q);
    g_free (controller_q);
  }
  output = run_command (cmd);
  if (output != NULL)
    {
      on = (strstr (output, "Powered: yes") != NULL);
      g_free (output);
    }
  g_free (cmd);
  return on;
}

void
bt_set_powered (gboolean on)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 1 power %s",
                                on ? "on" : "off");

  spawn_command (cmd);
  g_free (cmd);
}

/* Fills paired/trusted/connected flags via "info MAC". WORKER ONLY. */
void
bt_fill_flags (BtDevice *dev)
{
  gchar *mac_q = g_shell_quote (dev->mac);
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 1 info %s", mac_q);
  gchar *output;

  g_free (mac_q);
  output = run_command (cmd);

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
 * bluetoothctl exits 0 even on logical failures, so the text is parsed.
 * "Connection successful"/"AlreadyExists" count as success even when
 * wrapped in a "Failed to ..." line (pair on an already-paired device). */
gboolean
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

gboolean
bt_output_is_error (const gchar *output,
                    const gchar *errout)
{
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

typedef enum
{
  BT_FLAG_PAIRED,
  BT_FLAG_TRUSTED,
  BT_FLAG_CONNECTED
} BtFlagKind;

static gboolean
canon_hex12 (const gchar *s,
             gchar        out[13])
{
  gsize        len = 0;
  const gchar *p;

  for (p = s; *p != '\0'; p++)
    {
      if (*p == '-' || *p == ':')
        continue;
      if (len >= 12 || !g_ascii_isxdigit (*p))
        return FALSE;
      out[len++] = *p;
    }
  if (len != 12)
    return FALSE;
  out[12] = '\0';
  return TRUE;
}

static gboolean
name_is_mac_alias (const gchar *name,
                   const gchar *mac)
{
  gchar canon_name[13];
  gchar canon_mac[13];

  if (!canon_hex12 (name, canon_name))
    return FALSE;
  if (!canon_hex12 (mac, canon_mac))
    return FALSE;
  return g_ascii_strcasecmp (canon_name, canon_mac) == 0;
}

/* Parses one `bluetoothctl devices` line. On success stores stripped
 * MAC and name copies in `out_mac`/`out_name` (caller frees both)
 * and returns TRUE. Returns FALSE for lines without a usable name:
 * missing parts, empty name, name equal to MAC (case-insensitive)
 * or "Unknown"/"(unknown)" placeholders (case-insensitive). */
static gboolean
bt_parse_device_line (const gchar  *line,
                      gchar       **out_mac,
                      gchar       **out_name)
{
  gchar    **parts;
  gchar     *mac;
  gchar     *name;

  if (line == NULL)
    return FALSE;
  if (!g_str_has_prefix (line, "Device "))
    return FALSE;
  parts = g_strsplit (line, " ", 3);
  if (parts[1] == NULL || parts[2] == NULL)
    {
      g_strfreev (parts);
      return FALSE;
    }
  mac = g_strdup (parts[1]);
  name = g_strdup (parts[2]);
  g_strfreev (parts);
  g_strstrip (mac);
  g_strstrip (name);
  if (*mac == '\0' || *name == '\0')
    {
      g_free (mac);
      g_free (name);
      return FALSE;
    }
  if (g_ascii_strcasecmp (name, mac) == 0)
    {
      g_free (mac);
      g_free (name);
      return FALSE;
    }
  if (name_is_mac_alias (name, mac))
    {
      g_free (mac);
      g_free (name);
      return FALSE;
    }
  if (g_ascii_strcasecmp (name, "Unknown") == 0
      || g_ascii_strcasecmp (name, "(unknown)") == 0)
    {
      g_free (mac);
      g_free (name);
      return FALSE;
    }
  if (out_mac != NULL)
    *out_mac = mac;
  else
    g_free (mac);
  if (out_name != NULL)
    *out_name = name;
  else
    g_free (name);
  return TRUE;
}

static BtDevice *
find_device_by_mac (GList *devs, const gchar *mac)
{
  GList *l;

  for (l = devs; l != NULL; l = l->next)
    {
      BtDevice *candidate = l->data;

      if (g_strcmp0 (candidate->mac, mac) == 0)
        return candidate;
    }
  return NULL;
}

static void
apply_device_flag (BtDevice *dev, BtFlagKind flag)
{
  switch (flag)
    {
    case BT_FLAG_PAIRED:
      dev->paired = TRUE;
      break;
    case BT_FLAG_TRUSTED:
      dev->trusted = TRUE;
      break;
    case BT_FLAG_CONNECTED:
      dev->connected = TRUE;
      break;
    }
}

/* Applies one filtered listing (`devices Paired|Trusted|Connected`) to `devs`,
 * creating entries that are not yet present and setting the matching flag.
 * This avoids per-device `info` calls, which dominate the load latency. */
static void
bt_apply_filtered_devices (GList    **devs,
                          const gchar *filter,
                          BtFlagKind   flag)
{
  gchar  *cmd;
  gchar  *output;
  gchar **lines;
  gint    i;

  {
    gchar *filter_q = g_shell_quote (filter);

    cmd = g_strdup_printf ("bluetoothctl --timeout 1 devices %s", filter_q);
    g_free (filter_q);
  }
  output = run_command (cmd);
  g_free (cmd);
  if (output == NULL)
    return;

  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar    *mac = NULL;
      gchar    *name = NULL;
      BtDevice *dev;

      if (!bt_parse_device_line (lines[i], &mac, &name))
        continue;
      dev = find_device_by_mac (*devs, mac);
      if (dev == NULL)
        {
          dev = g_new0 (BtDevice, 1);
          dev->mac = g_strdup (mac);
          dev->name = g_strdup (name);
          *devs = g_list_append (*devs, dev);
        }
      else if (dev->name == NULL)
        dev->name = g_strdup (name);
      apply_device_flag (dev, flag);
      g_free (mac);
      g_free (name);
    }
  g_strfreev (lines);
  g_free (output);
}

GList *
list_bt_devices (void)
{
  gchar  *output;
  gchar **lines;
  GList  *devs = NULL;
  gint    i;

  /* Base list of (recently) known devices, without per-device `info`. */
  output = run_command ("bluetoothctl --timeout 1 devices");
  if (output != NULL)
    {
      lines = g_strsplit (output, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          gchar    *mac = NULL;
          gchar    *name = NULL;
          BtDevice *dev;

          if (!bt_parse_device_line (lines[i], &mac, &name))
            continue;
          dev = g_new0 (BtDevice, 1);
          dev->mac = g_strdup (mac);
          dev->name = g_strdup (name);
          devs = g_list_append (devs, dev);
          g_free (mac);
          g_free (name);
        }
      g_strfreev (lines);
      g_free (output);
    }

  /* Flags come from filtered listings; much cheaper than `info MAC`. */
  bt_apply_filtered_devices (&devs, "Paired", BT_FLAG_PAIRED);
  bt_apply_filtered_devices (&devs, "Trusted", BT_FLAG_TRUSTED);
  bt_apply_filtered_devices (&devs, "Connected", BT_FLAG_CONNECTED);

  return devs;
}

static gboolean
path_suffix_is_device (const gchar  *object_path,
                       const gchar  *mac,
                       gchar       **norm_out)
{
  const gchar *base;
  const gchar *dev;
  gchar       *norm;
  gchar       *p;

  if (object_path == NULL || mac == NULL || norm_out == NULL)
    return FALSE;
  if (*object_path == '\0' || *mac == '\0')
    return FALSE;
  base = strrchr (object_path, '/');
  if (base != NULL)
    base++;
  else
    base = object_path;
  if (g_ascii_strncasecmp (base, "dev_", 4) != 0)
    return FALSE;
  dev = base + 4;
  if (*dev == '\0')
    return FALSE;
  norm = g_strdup (mac);
  for (p = norm; *p != '\0'; p++)
    {
      if (*p == ':')
        *p = '_';
    }
  if (g_ascii_strcasecmp (dev, norm) != 0)
    {
      g_free (norm);
      return FALSE;
    }
  *norm_out = norm;
  return TRUE;
}

static gboolean
connected_from_changed (GVariant *changed,
                        gboolean *connected_out)
{
  GVariant *val = g_variant_lookup_value (changed, "Connected", NULL);
  GVariant *inner = NULL;
  gboolean  connected = FALSE;
  gboolean  have = FALSE;

  if (val == NULL)
    return FALSE;
  if (g_variant_is_of_type (val, G_VARIANT_TYPE_VARIANT))
    {
      inner = g_variant_get_variant (val);
      if (g_variant_is_of_type (inner, G_VARIANT_TYPE_BOOLEAN))
        {
          connected = g_variant_get_boolean (inner);
          have = TRUE;
        }
      g_variant_unref (inner);
    }
  else if (g_variant_is_of_type (val, G_VARIANT_TYPE_BOOLEAN))
    {
      connected = g_variant_get_boolean (val);
      have = TRUE;
    }
  g_variant_unref (val);
  if (!have)
    return FALSE;
  *connected_out = connected;
  return TRUE;
}

/* Matches the received object path suffix against the link MAC and
 * extracts Connected from the changed-properties dictionary. The
 * adapter prefix (hci0, hci1, ...) is ignored because only the text
 * after the last slash is compared. Comparison is case-insensitive
 * and colons in the MAC compare equal to underscores in the path. */
BtSignalMatch
bt_signal_event_matches (const gchar *object_path,
                         const gchar *mac,
                         GVariant    *changed)
{
  gchar    *norm = NULL;
  gboolean  connected = FALSE;

  if (object_path == NULL || mac == NULL || changed == NULL)
    return BT_SIGNAL_NO_MATCH;
  if (*mac == '\0' || *object_path == '\0')
    return BT_SIGNAL_NO_MATCH;
  if (!g_variant_is_of_type (changed, G_VARIANT_TYPE_VARDICT))
    return BT_SIGNAL_NO_MATCH;
  if (!path_suffix_is_device (object_path, mac, &norm))
    return BT_SIGNAL_NO_MATCH;
  g_free (norm);
  if (!connected_from_changed (changed, &connected))
    return BT_SIGNAL_NO_MATCH;
  if (connected)
    return BT_SIGNAL_CONNECTED_TRUE;
  return BT_SIGNAL_CONNECTED_FALSE;
}

/* --- Trust standalone (RF-006) and rename (RF-007) ---
 * Helpers never touch GTK. MAC and alias are validated and
 * shell-quoted; outputs are classified with bt_str_contains_ci so
 * AlreadyExists counts as success and auth/agent failures keep
 * their own codes for UI feedback. */

gboolean
bt_mac_is_valid (const gchar *mac)
{
  gint i;

  if (mac == NULL)
    return FALSE;
  if (strlen (mac) != 17)
    return FALSE;
  for (i = 0; i < 17; i++)
    {
      gchar c = mac[i];

      if (i % 3 == 2)
        {
          if (c != ':')
            return FALSE;
        }
      else if (!g_ascii_isxdigit (c))
        return FALSE;
    }
  return TRUE;
}

gboolean
bt_alias_is_valid (const gchar *alias)
{
  gchar  *copy;
  gchar  *stripped;
  gsize   len;
  gboolean ok = FALSE;

  if (alias == NULL)
    return FALSE;
  copy = g_strdup (alias);
  stripped = g_strstrip (copy);
  len = strlen (stripped);
  if (len == 0 || len > 128)
    {
      g_free (copy);
      return FALSE;
    }
  if (strchr (stripped, '\n') != NULL || strchr (stripped, '\r') != NULL)
    {
      g_free (copy);
      return FALSE;
    }
  ok = TRUE;
  g_free (copy);
  return ok;
}

gchar *
bt_build_trust_command (const gchar *mac)
{
  gchar *mac_q = NULL;
  gchar *cmd = NULL;

  if (!bt_mac_is_valid (mac))
    return NULL;
  mac_q = g_shell_quote (mac);
  cmd = g_strdup_printf ("bluetoothctl --timeout 5 trust %s", mac_q);
  g_free (mac_q);
  return cmd;
}

gchar *
bt_build_rename_command (const gchar *mac,
                         const gchar *alias)
{
  gchar *mac_q = NULL;
  gchar *alias_q = NULL;
  gchar *cmd = NULL;

  if (!bt_mac_is_valid (mac) || !bt_alias_is_valid (alias))
    return NULL;
  mac_q = g_shell_quote (mac);
  alias_q = g_shell_quote (alias);
  cmd = g_strdup_printf ("bluetoothctl --timeout 5 set-alias %s %s",
                         mac_q, alias_q);
  g_free (mac_q);
  g_free (alias_q);
  return cmd;
}

BtTrustStatus
bt_classify_trust_output (const gchar *output,
                          const gchar *errout)
{
  if (bt_str_contains_ci (output, "successful")
      || bt_str_contains_ci (errout, "successful")
      || bt_str_contains_ci (output, "succeeded")
      || bt_str_contains_ci (errout, "succeeded")
      || bt_str_contains_ci (output, "alreadyexists")
      || bt_str_contains_ci (errout, "alreadyexists"))
    return BT_TRUST_OK;
  if (bt_str_contains_ci (output, "authenticationfailed")
      || bt_str_contains_ci (errout, "authenticationfailed")
      || bt_str_contains_ci (output, "authentication failed")
      || bt_str_contains_ci (errout, "authentication failed"))
    return BT_TRUST_AUTH_FAILED;
  if (bt_str_contains_ci (output, "no agent")
      || bt_str_contains_ci (errout, "no agent"))
    return BT_TRUST_NO_AGENT;
  return BT_TRUST_FAILED;
}

BtRenameStatus
bt_classify_rename_output (const gchar *output,
                            const gchar *errout)
{
  if (bt_str_contains_ci (output, "successful")
      || bt_str_contains_ci (errout, "successful")
      || bt_str_contains_ci (output, "succeeded")
      || bt_str_contains_ci (errout, "succeeded")
      || bt_str_contains_ci (output, "alreadyexists")
      || bt_str_contains_ci (errout, "alreadyexists")
      || bt_str_contains_ci (output, "changed")
      || bt_str_contains_ci (errout, "changed"))
    return BT_RENAME_OK;
  if (bt_str_contains_ci (output, "authenticationfailed")
      || bt_str_contains_ci (errout, "authenticationfailed")
      || bt_str_contains_ci (output, "authentication failed")
      || bt_str_contains_ci (errout, "authentication failed"))
    return BT_RENAME_AUTH_FAILED;
  if (bt_str_contains_ci (output, "no agent")
      || bt_str_contains_ci (errout, "no agent"))
    return BT_RENAME_NO_AGENT;
  if (bt_str_contains_ci (output, "no such device")
      || bt_str_contains_ci (errout, "no such device")
      || bt_str_contains_ci (output, "not available")
      || bt_str_contains_ci (errout, "not available")
      || bt_str_contains_ci (output, "unknown device")
      || bt_str_contains_ci (errout, "unknown device"))
    return BT_RENAME_NO_DEVICE;
  return BT_RENAME_FAILED;
}

BtTrustStatus
bt_trust_device (const gchar *mac)
{
  gchar         *cmd = NULL;
  gchar         *output = NULL;
  gchar         *errout = NULL;
  gint           status = 0;
  gboolean       spawned = FALSE;
  BtTrustStatus  result = BT_TRUST_FAILED;

  cmd = bt_build_trust_command (mac);
  if (cmd == NULL)
    return BT_TRUST_FAILED;
  spawned = g_spawn_command_line_sync (cmd, &output, &errout,
                                       &status, NULL);
  g_free (cmd);
  if (!spawned)
    {
      g_free (output);
      g_free (errout);
      return BT_TRUST_FAILED;
    }
  result = bt_classify_trust_output (output, errout);
  g_free (output);
  g_free (errout);
  return result;
}

gboolean
bt_rename_via_dbus (const gchar *mac,
                    const gchar *alias,
                    GError     **error)
{
  GDBusConnection *conn = NULL;
  GVariant        *params = NULL;
  GVariant        *result = NULL;
  gchar           *path = NULL;
  gchar            dev[33];
  gsize            j = 0;
  const gchar     *s;

  if (!bt_mac_is_valid (mac) || !bt_alias_is_valid (alias))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "bt_rename_via_dbus: invalid mac or alias");
      return FALSE;
    }
  for (s = mac; *s != '\0'; s++)
    {
      if (*s == ':')
        {
          if (j >= sizeof (dev) - 1)
            break;
          dev[j++] = '_';
        }
      else
        {
          if (j >= sizeof (dev) - 1)
            break;
          dev[j++] = (gchar) g_ascii_toupper (*s);
        }
    }
  dev[j] = '\0';
  path = g_strdup_printf ("/org/bluez/hci0/dev_%s", dev);
  conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (conn == NULL)
    {
      g_free (path);
      return FALSE;
    }
  params = g_variant_new ("(ssv)",
                          "org.bluez.Device1",
                          "Alias",
                          g_variant_new_string (alias));
  result = g_dbus_connection_call_sync (conn,
                                        "org.bluez",
                                        path,
                                        "org.freedesktop.DBus.Properties",
                                        "Set",
                                        params,
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        5000,
                                        NULL,
                                        error);
  g_free (path);
  g_object_unref (conn);
  if (result == NULL)
    return FALSE;
  g_variant_unref (result);
  return TRUE;
}

BtRenameDbusFn bt_rename_dbus_fn = bt_rename_via_dbus;

void
bt_rename_set_dbus_fn (BtRenameDbusFn fn)
{
  if (fn == NULL)
    bt_rename_dbus_fn = bt_rename_via_dbus;
  else
    bt_rename_dbus_fn = fn;
}

BtRenameStatus
bt_rename_device (const gchar *mac,
                  const gchar *alias)
{
  gchar          *cmd = NULL;
  gchar          *output = NULL;
  gchar          *errout = NULL;
  gint            status = 0;
  gboolean        spawned = FALSE;
  BtRenameStatus  primary = BT_RENAME_FAILED;

  cmd = bt_build_rename_command (mac, alias);
  if (cmd == NULL)
    return BT_RENAME_FAILED;
  spawned = g_spawn_command_line_sync (cmd, &output, &errout,
                                       &status, NULL);
  g_free (cmd);
  if (!spawned)
    {
      g_free (output);
      g_free (errout);
      return BT_RENAME_FAILED;
    }
  primary = bt_classify_rename_output (output, errout);
  g_free (output);
  g_free (errout);
  if (primary == BT_RENAME_OK)
    return BT_RENAME_OK;
  if (bt_rename_dbus_fn != NULL && bt_rename_dbus_fn (mac, alias, NULL))
    return BT_RENAME_OK;
  return primary;
}
