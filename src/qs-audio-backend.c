/* qs-audio-backend.c — Audio backend (pactl/wpctl/amixer) movido de qs-audio.c.
 * Movimento puro, zero lógica; UI permanece em qs-audio.c.
 * NOTA: plano citava "move_streams_to_target" — nome real verificado no
 * arquivo é pactl_move_streams. */

#include "qs-audio-backend.h"
#include "qs-util.h"

void
audio_dev_free (gpointer data)
{
  AudioDev *dev = data;

  g_free (dev->id);
  g_free (dev->label);
  g_free (dev);
}

/* Volume backend: pactl > wpctl > amixer.
 * Backend detectado 1x e cacheado (nenhum command_exists/spawn na UI
 * depois da primeira chamada). Leitura roda em worker
 * (run_command_async / GThread); fill dos widgets sempre no idle/main. */

static AudioBackend audio_cached_backend = AUDIO_NONE;
static gboolean audio_backend_probed = FALSE;
static gboolean audio_have_amixer = FALSE;

AudioBackend
audio_get_backend_cached (void)
{
  if (!audio_backend_probed)
    {
      if (command_exists ("pactl"))
        audio_cached_backend = AUDIO_PACTL;
      else if (command_exists ("wpctl"))
        audio_cached_backend = AUDIO_WPCTL;
      else
        audio_cached_backend = AUDIO_NONE;
      audio_have_amixer = command_exists ("amixer");
      audio_backend_probed = TRUE;
    }
  return audio_cached_backend;
}

/* Leitura do flag audio_have_amixer para a UI (audio_refresh_volume),
 * que permanece em qs-audio.c após o split. */
gboolean
audio_have_amixer_avail (void)
{
  (void) audio_get_backend_cached ();
  return audio_have_amixer;
}

void
set_volume_percent (gint value)
{
  gchar *cmd;

  value = CLAMP (value, 0, 150);
  switch (audio_get_backend_cached ())
    {
    case AUDIO_PACTL:
      cmd = g_strdup_printf ("pactl set-sink-volume @DEFAULT_SINK@ %d%%", value);
      break;
    case AUDIO_WPCTL:
      cmd = g_strdup_printf ("wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", value);
      break;
    default:
      if (!audio_have_amixer)
        return;
      cmd = g_strdup_printf ("amixer -q sset Master %d%%", CLAMP (value, 0, 100));
      break;
    }
  spawn_command (cmd);
  g_free (cmd);
}

const gchar *
volume_icon_for_level (gint percent)
{
  if (percent > 100)
    return "audio-volume-overamplified-symbolic";
  if (percent <= 0)
    return "audio-volume-muted-symbolic";
  if (percent <= 33)
    return "audio-volume-low-symbolic";
  if (percent <= 66)
    return "audio-volume-medium-symbolic";
  return "audio-volume-high-symbolic";
}
/* Audio device listing: pactl preferred, wpctl fallback.
 * Combo id = pactl name or wpctl number. */

/* Maps pactl device name -> Description from "pactl list sinks/sources".
 * block_prefix is "Sink #" or "Source #". */
static GHashTable *
parse_pactl_descriptions (const gchar *output,
                          const gchar *block_prefix)
{
  GHashTable *map;
  gchar      **lines;
  gchar       *name = NULL;
  gint         i;

  map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (output == NULL)
    return map;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      if (g_str_has_prefix (lines[i], block_prefix))
        {
          g_free (name);
          name = NULL;
        }
      else if (name == NULL && g_str_has_prefix (lines[i], "\tName: "))
        name = g_strdup (lines[i] + strlen ("\tName: "));
      else if (name != NULL
               && g_str_has_prefix (lines[i], "\tDescription: "))
        {
          g_hash_table_insert (map, name,
                               g_strdup (lines[i] + strlen ("\tDescription: ")));
          name = NULL;
        }
    }
  g_free (name);
  g_strfreev (lines);
  return map;
}

/* Set of source names that are monitors (not real microphones). */
GHashTable *
parse_pactl_monitor_sources (const gchar *output)
{
  GHashTable *monitors;
  gchar      **lines;
  gchar       *name = NULL;
  const gchar *value;
  gint         i;

  monitors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (output == NULL)
    return monitors;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      if (g_str_has_prefix (lines[i], "Source #"))
        {
          g_free (name);
          name = NULL;
        }
      else if (name == NULL && g_str_has_prefix (lines[i], "\tName: "))
        name = g_strdup (lines[i] + strlen ("\tName: "));
      else if (name != NULL
               && g_str_has_prefix (lines[i], "\tMonitor of Sink: "))
        {
          value = lines[i] + strlen ("\tMonitor of Sink: ");
          if (g_strcmp0 (value, "n/a") != 0)
            g_hash_table_insert (monitors, g_strdup (name),
                                 GINT_TO_POINTER (TRUE));
          g_free (name);
          name = NULL;
        }
    }
  g_free (name);
  g_strfreev (lines);
  return monitors;
}

AudioDev *
audio_dev_new (const gchar *id,
               const gchar *label,
               gboolean     is_default)
{
  AudioDev *dev = g_new0 (AudioDev, 1);

  dev->id = g_strdup (id);
  dev->label = g_strdup (label != NULL ? label : id);
  dev->is_default = is_default;
  return dev;
}

gchar *
pactl_get_default (const gchar *cmd)
{
  gchar *output = run_command (cmd);
  gchar *def;

  if (output == NULL)
    return NULL;
  def = g_strdup (g_strstrip (output));
  g_free (output);
  return def;
}

GList *
list_pactl_sinks (void)
{
  GList      *devs = NULL;
  gchar      *short_out;
  gchar      *full_out;
  GHashTable *descs;
  gchar      *def;
  gchar     **lines;
  gint        i;

  short_out = run_command ("pactl list short sinks");
  if (short_out == NULL)
    return NULL;
  full_out = run_command ("pactl list sinks");
  descs = parse_pactl_descriptions (full_out, "Sink #");
  def = pactl_get_default ("pactl get-default-sink");

  lines = g_strsplit (short_out, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar **fields;
      const gchar *name;
      const gchar *desc;

      if (*lines[i] == '\0')
        continue;
      fields = g_strsplit (lines[i], "\t", 0);
      if (g_strv_length (fields) < 2)
        {
          g_strfreev (fields);
          continue;
        }
      name = fields[1];
      desc = g_hash_table_lookup (descs, name);
      devs = g_list_append (devs, audio_dev_new (name,
                                                desc != NULL ? desc : name,
                                                def != NULL
                                                && g_strcmp0 (def, name) == 0));
      g_strfreev (fields);
    }
  g_strfreev (lines);
  g_hash_table_destroy (descs);
  g_free (full_out);
  g_free (short_out);
  g_free (def);
  return devs;
}

GList *
list_pactl_sources (void)
{
  GList      *devs = NULL;
  gchar      *short_out;
  gchar      *full_out;
  GHashTable *descs;
  GHashTable *monitors;
  gchar      *def;
  gchar     **lines;
  gint        i;

  short_out = run_command ("pactl list short sources");
  if (short_out == NULL)
    return NULL;
  full_out = run_command ("pactl list sources");
  descs = parse_pactl_descriptions (full_out, "Source #");
  monitors = parse_pactl_monitor_sources (full_out);
  def = pactl_get_default ("pactl get-default-source");

  lines = g_strsplit (short_out, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar **fields;
      const gchar *name;
      const gchar *desc;

      if (*lines[i] == '\0')
        continue;
      fields = g_strsplit (lines[i], "\t", 0);
      if (g_strv_length (fields) < 2)
        {
          g_strfreev (fields);
          continue;
        }
      name = fields[1];
      if (!g_str_has_suffix (name, ".monitor")
          && !g_hash_table_contains (monitors, name))
        {
          desc = g_hash_table_lookup (descs, name);
          devs = g_list_append (devs, audio_dev_new (name,
                                                    desc != NULL ? desc : name,
                                                    def != NULL
                                                    && g_strcmp0 (def,
                                                                  name) == 0));
        }
      g_strfreev (fields);
    }
  g_strfreev (lines);
  g_hash_table_destroy (monitors);
  g_hash_table_destroy (descs);
  g_free (full_out);
  g_free (short_out);
  g_free (def);
  return devs;
}

GList *
list_sinks (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sinks ();
  return NULL;
}

GList *
list_sources (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sources ();
  return NULL;
}

/* Move active streams to target so the switch applies immediately
 * (set-default alone only affects new streams). pactl only. */
void
pactl_move_streams (const gchar *short_list_cmd,
                    const gchar *move_cmd,
                    const gchar *target)
{
  gchar *output = run_command (short_list_cmd);
  gchar **lines;
  gchar  *quoted;
  gint    i;

  if (output == NULL)
    return;
  quoted = g_shell_quote (target);
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar *end = NULL;
      glong  id = strtol (lines[i], &end, 10);
      gchar *cmd;

      if (end == lines[i] || id < 0
          || (*end != '\0' && *end != '\t' && *end != ' '))
        continue;
      cmd = g_strdup_printf ("%s %ld %s", move_cmd, id, quoted);
      spawn_command (cmd);
      g_free (cmd);
    }
  g_strfreev (lines);
  g_free (quoted);
  g_free (output);
}
