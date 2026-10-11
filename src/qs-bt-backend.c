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
  gchar  *output = run_command ("bluetoothctl --timeout 2 list");
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

void
bt_set_powered (gboolean on)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 2 power %s",
                                on ? "on" : "off");

  spawn_command (cmd);
  g_free (cmd);
}

/* Fills paired/trusted/connected flags via "info MAC". WORKER ONLY. */
void
bt_fill_flags (BtDevice *dev)
{
  gchar *cmd = g_strdup_printf ("bluetoothctl --timeout 2 info %s",
                                dev->mac);
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

GList *
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
      devs = g_list_prepend (devs, dev);
      g_strfreev (parts);
    }
  g_strfreev (lines);
  g_free (output);
  return g_list_reverse (devs);
}
