/* qs-wifi-backend.c — Wi-Fi backend (nmcli) movido de qs-wifi.c.
 * Movimento puro; cache wifi_saved_* permanece em qs-wifi.c. */

#include "qs-wifi-backend.h"
#include "qs-util.h"

void
wifi_net_free (gpointer data)
{
  WifiNet *net = data;

  g_free (net->ssid);
  g_free (net);
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

void
wifi_set_enabled (gboolean on)
{
  gchar *cmd = g_strdup_printf ("nmcli radio wifi %s", on ? "on" : "off");

  spawn_command (cmd);
  g_free (cmd);
}

/* Parses "IN-USE,SSID,SIGNAL,SECURITY" terse output. Dedupes by SSID (one
 * row per SSID): keeps the strongest signal, marks active if any BSSID
 * is current. prepend+reverse keeps list order without O(n^2) append. */
GList *
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
      if (!qs_parse_int_checked (fields[2], &signal))
        signal = 0;
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

const gchar *
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
