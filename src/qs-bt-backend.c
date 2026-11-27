#include "qs-bt-backend.h"
#include "qs-util.h"

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
  {
    gchar canon_name[13];
    gchar canon_mac[13];
    gsize name_len = 0;
    gsize mac_len = 0;
    gboolean both_hex = TRUE;
    const gchar *s;

    for (s = name; *s != '\0'; s++)
      {
        if (*s == '-' || *s == ':')
          continue;
        if (name_len >= 12 || !g_ascii_isxdigit (*s))
          {
            both_hex = FALSE;
            break;
          }
        canon_name[name_len++] = *s;
      }
    if (both_hex)
      {
        for (s = mac; *s != '\0'; s++)
          {
            if (*s == '-' || *s == ':')
              continue;
            if (mac_len >= 12 || !g_ascii_isxdigit (*s))
              {
                both_hex = FALSE;
                break;
              }
            canon_mac[mac_len++] = *s;
          }
      }
    if (both_hex && name_len == 12 && mac_len == 12)
      {
        canon_name[12] = '\0';
        canon_mac[12] = '\0';
        if (g_ascii_strcasecmp (canon_name, canon_mac) == 0)
          {
            g_free (mac);
            g_free (name);
            return FALSE;
          }
      }
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
      gchar      *mac = NULL;
      gchar      *name = NULL;
      BtDevice   *dev = NULL;
      GList      *l;

      if (!bt_parse_device_line (lines[i], &mac, &name))
        continue;

      for (l = *devs; l != NULL; l = l->next)
        {
          BtDevice *candidate = l->data;

          if (g_strcmp0 (candidate->mac, mac) == 0)
            {
              dev = candidate;
              break;
            }
        }
      if (dev == NULL)
        {
          dev = g_new0 (BtDevice, 1);
          dev->mac = g_strdup (mac);
          dev->name = g_strdup (name);
          *devs = g_list_append (*devs, dev);
        }
      else if (dev->name == NULL)
        dev->name = g_strdup (name);

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
  const gchar *base;
  const gchar *dev;
  gchar       *norm = NULL;
  gchar       *p;
  GVariant    *val = NULL;
  GVariant    *inner = NULL;
  gboolean     connected = FALSE;
  gboolean     have = FALSE;

  if (object_path == NULL || mac == NULL || changed == NULL)
    return BT_SIGNAL_NO_MATCH;
  if (*mac == '\0' || *object_path == '\0')
    return BT_SIGNAL_NO_MATCH;
  if (!g_variant_is_of_type (changed, G_VARIANT_TYPE_VARDICT))
    return BT_SIGNAL_NO_MATCH;
  base = strrchr (object_path, '/');
  if (base != NULL)
    base++;
  else
    base = object_path;
  if (g_ascii_strncasecmp (base, "dev_", 4) != 0)
    return BT_SIGNAL_NO_MATCH;
  dev = base + 4;
  if (*dev == '\0')
    return BT_SIGNAL_NO_MATCH;
  norm = g_strdup (mac);
  for (p = norm; *p != '\0'; p++)
    {
      if (*p == ':')
        *p = '_';
    }
  if (g_ascii_strcasecmp (dev, norm) != 0)
    {
      g_free (norm);
      return BT_SIGNAL_NO_MATCH;
    }
  g_free (norm);
  val = g_variant_lookup_value (changed, "Connected", NULL);
  if (val == NULL)
    return BT_SIGNAL_NO_MATCH;
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
    return BT_SIGNAL_NO_MATCH;
  if (connected)
    return BT_SIGNAL_CONNECTED_TRUE;
  return BT_SIGNAL_CONNECTED_FALSE;
}
