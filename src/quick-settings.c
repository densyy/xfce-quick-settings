#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
  AUDIO_NONE = 0,
  AUDIO_PACTL,
  AUDIO_WPCTL
} AudioBackend;

typedef struct
{
  XfcePanelPlugin *plugin;
  GtkWidget       *button;
  GtkWidget       *button_image;
  GtkWidget       *popup;
  GtkWidget       *vol_icon;
  GtkWidget       *vol_scale;
  GtkWidget       *bright_icon;
  GtkWidget       *bright_scale;
  GtkWidget       *sink_combo;
  GtkWidget       *source_combo;
  AudioBackend     audio_backend;
  GtkWidget       *stack;
  GtkWidget       *wifi_tile;
  GtkWidget       *bt_tile;
  GtkWidget       *wifi_switch;
  GtkWidget       *wifi_list;
  GtkWidget       *wifi_scan;
  GtkWidget       *wifi_status;
  GtkWidget       *bt_switch;
  GtkWidget       *bt_list;
  GtkWidget       *bt_scan;
  GtkWidget       *bt_status;
  GtkWidget       *night_day;
  GtkWidget       *night_aft;
  GtkWidget       *night_night;
  GtkWidget       *bat_box;
  GtkWidget       *bat_icon;
  GtkWidget       *bat_label;
  gboolean         wifi_on;  gboolean         bt_on; /* cached: live check may block without daemon */
  gchar           *bt_controller; /* owned MAC or NULL */
  gboolean         scanning_wifi;
  gboolean         scanning_bt;
  gboolean         updating; /* TRUE while loading current values */
  gint             refcount; /* async ops hold a ref; free at zero */
  gboolean         dead; /* TRUE after free-data: don't touch widgets */
} QuickSettings;

/* One audio device: combo id + visible label + default flag. */
typedef struct
{
  gchar    *id;
  gchar    *label;
  gboolean  is_default;
} AudioDev;

static void
audio_dev_free (gpointer data)
{
  AudioDev *dev = data;

  g_free (dev->id);
  g_free (dev->label);
  g_free (dev);
}

static void
qs_ref (QuickSettings *qs)
{
  g_atomic_int_inc (&qs->refcount);
}

static void
qs_unref (QuickSettings *qs)
{
  if (g_atomic_int_dec_and_test (&qs->refcount))
    {
      g_free (qs->bt_controller);
      g_free (qs);
    }
}

/* ---------------- command helpers ---------------- */

static gboolean
command_exists (const gchar *name)
{
  gchar *path = g_find_program_in_path (name);

  if (path != NULL)
    {
      g_free (path);
      return TRUE;
    }
  return FALSE;
}

/* Runs cmdline synchronously, returns stdout (caller frees) or NULL. */
static gchar *
run_command (const gchar *cmdline)
{
  gchar  *output = NULL;
  GError *error = NULL;

  if (!g_spawn_command_line_sync (cmdline, &output, NULL, NULL, &error))
    {
      g_clear_error (&error);
      return NULL;
    }
  return output;
}

static void
spawn_command (const gchar *cmdline)
{
  GError *error = NULL;

  if (!g_spawn_command_line_async (cmdline, &error))
    g_clear_error (&error);
}

/* Async command: runs in a worker thread, done() fires in the main loop. */
typedef void (*QsCmdDone) (const gchar *output,
                           const gchar *errout,
                           gboolean     ok,
                           gpointer     user_data);

typedef struct
{
  gchar    *cmdline;
  QsCmdDone done;
  gpointer  user_data;
  gchar    *output;
  gchar    *errout;
  gboolean  ok;
} QsAsync;

static gboolean
qs_async_idle (gpointer data)
{
  QsAsync *a = data;

  a->done (a->output, a->errout, a->ok, a->user_data);
  g_free (a->cmdline);
  g_free (a->output);
  g_free (a->errout);
  g_free (a);
  return G_SOURCE_REMOVE;
}

static gpointer
qs_async_thread (gpointer data)
{
  QsAsync *a = data;
  GError  *error = NULL;

  a->ok = g_spawn_command_line_sync (a->cmdline, &a->output, &a->errout,
                                     NULL, &error);
  if (error != NULL)
    g_clear_error (&error);
  g_idle_add (qs_async_idle, a);
  return NULL;
}

static void
run_command_async (const gchar *cmdline,
                   QsCmdDone    done,
                   gpointer     user_data)
{
  QsAsync *a = g_new0 (QsAsync, 1);

  a->cmdline = g_strdup (cmdline);
  a->done = done;
  a->user_data = user_data;
  g_thread_unref (g_thread_new ("qs-cmd", qs_async_thread, a));
}

/* Split an nmcli terse line on unescaped ':' (handles \: and \\). */
static gchar **
split_terse_line (const gchar *line,
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

/* First "NN%" found in text, or -1. */
static gint
parse_first_percent (const gchar *text)
{
  const gchar *p;
  const gchar *start;
  gchar       *num;
  gint         value;

  if (text == NULL)
    return -1;
  p = strchr (text, '%');
  if (p == NULL)
    return -1;
  start = p;
  while (start > text && g_ascii_isdigit (*(start - 1)))
    start--;
  if (start == p)
    return -1;
  num = g_strndup (start, (gsize) (p - start));
  value = atoi (num);
  g_free (num);
  return value;
}

/* ---------------- volume backend: pactl > wpctl > amixer ---------------- */

static gint
get_volume_percent (void)
{
  gchar       *output;
  gint         value = -1;
  const gchar *p;

  if (command_exists ("pactl"))
    {
      output = run_command ("pactl get-sink-volume @DEFAULT_SINK@");
      value = parse_first_percent (output);
      g_free (output);
      if (value >= 0)
        return value;
    }
  if (command_exists ("wpctl"))
    {
      output = run_command ("wpctl get-volume @DEFAULT_AUDIO_SINK@");
      if (output != NULL)
        {
          p = strstr (output, "Volume:");
          if (p != NULL)
            value = (gint) (g_ascii_strtod (p + 7, NULL) * 100.0 + 0.5);
          g_free (output);
          if (value >= 0)
            return value;
        }
    }
  if (command_exists ("amixer"))
    {
      output = run_command ("amixer get Master");
      value = parse_first_percent (output);
      g_free (output);
    }
  return value;
}

static void
set_volume_percent (gint value)
{
  gchar *cmd;

  value = CLAMP (value, 0, 150);
  if (command_exists ("pactl"))
    cmd = g_strdup_printf ("pactl set-sink-volume @DEFAULT_SINK@ %d%%", value);
  else if (command_exists ("wpctl"))
    cmd = g_strdup_printf ("wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", value);
  else if (command_exists ("amixer"))
    /* ALSA caps at 100%, no overamplification. */
    cmd = g_strdup_printf ("amixer -q sset Master %d%%", CLAMP (value, 0, 100));
  else
    return;
  spawn_command (cmd);
  g_free (cmd);
}

static const gchar *
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

/* ---------------- brightness backend ----------------
 * brightnessctl > sysfs > xbacklight > xrandr */

static gint
get_brightness_percent (void)
{
  gchar       *output;
  gchar      **parts;
  gint         value = -1;
  GDir        *dir;
  const gchar *entry;
  gchar       *bright_path;
  gchar       *max_path;
  gchar       *contents = NULL;
  gint         brightness;
  gint         max_brightness;
  const gchar *p;

  if (command_exists ("brightnessctl"))
    {
      output = run_command ("brightnessctl -m");
      if (output != NULL)
        {
          parts = g_strsplit (output, ",", 5);
          if (g_strv_length (parts) > 3)
            value = parse_first_percent (parts[3]);
          g_strfreev (parts);
          g_free (output);
          if (value >= 0)
            return CLAMP (value, 0, 100);
        }
    }

  dir = g_dir_open ("/sys/class/backlight", 0, NULL);
  if (dir != NULL)
    {
      entry = g_dir_read_name (dir);
      if (entry != NULL)
        {
          bright_path = g_build_filename ("/sys/class/backlight", entry,
                                           "brightness", NULL);
          max_path = g_build_filename ("/sys/class/backlight", entry,
                                        "max_brightness", NULL);
          if (g_file_get_contents (bright_path, &contents, NULL, NULL))
            {
              brightness = atoi (contents);
              g_free (contents);
              contents = NULL;
              if (g_file_get_contents (max_path, &contents, NULL, NULL))
                {
                  max_brightness = atoi (contents);
                  g_free (contents);
                  contents = NULL;
                  if (max_brightness > 0)
                    value = brightness * 100 / max_brightness;
                }
            }
          g_free (bright_path);
          g_free (max_path);
        }
      g_dir_close (dir);
      if (value >= 0)
        return CLAMP (value, 0, 100);
    }

  if (command_exists ("xbacklight"))
    {
      output = run_command ("xbacklight -get");
      if (output != NULL)
        {
          if (*output != '\0' && g_ascii_isdigit (*output))
            value = atoi (output);
          g_free (output);
          if (value >= 0)
            return CLAMP (value, 0, 100);
        }
    }

  if (command_exists ("xrandr"))
    {
      output = run_command ("xrandr --verbose");
      if (output != NULL)
        {
          p = strstr (output, "Brightness:");
          if (p != NULL)
            value = (gint) (g_ascii_strtod (p + 11, NULL) * 100.0 + 0.5);
          g_free (output);
        }
    }
  return (value >= 0) ? CLAMP (value, 0, 100) : -1;
}

/* First connected output from xrandr, or NULL (caller frees). */
static gchar *
get_xrandr_output (void)
{
  gchar  *output;
  gchar **lines;
  gchar  *name = NULL;
  gint    i;

  output = run_command ("xrandr --verbose");
  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      if (strstr (lines[i], " connected") != NULL)
        {
          gchar **tokens = g_strsplit (lines[i], " ", 2);

          if (tokens[0] != NULL && *tokens[0] != '\0')
            name = g_strdup (tokens[0]);
          g_strfreev (tokens);
          break;
        }
    }
  g_strfreev (lines);
  g_free (output);
  return name;
}

static void
set_brightness_percent (gint value)
{
  gchar       *cmd;
  GDir        *dir;
  const gchar *entry;
  gchar       *bright_path;
  gchar       *max_path;
  gchar       *contents = NULL;
  gint         max_brightness;
  gboolean     handled = FALSE;
  gchar       *name;

  value = CLAMP (value, 0, 100);

  if (command_exists ("brightnessctl"))
    {
      cmd = g_strdup_printf ("brightnessctl set %d%%", value);
      spawn_command (cmd);
      g_free (cmd);
      return;
    }

  dir = g_dir_open ("/sys/class/backlight", 0, NULL);
  if (dir != NULL)
    {
      entry = g_dir_read_name (dir);
      if (entry != NULL)
        {
          max_path = g_build_filename ("/sys/class/backlight", entry,
                                        "max_brightness", NULL);
          if (g_file_get_contents (max_path, &contents, NULL, NULL))
            {
              max_brightness = atoi (contents);
              g_free (contents);
              if (max_brightness > 0)
                {
                  bright_path = g_build_filename ("/sys/class/backlight",
                                                   entry, "brightness", NULL);
                  cmd = g_strdup_printf ("%d", value * max_brightness / 100);
                  g_file_set_contents (bright_path, cmd, -1, NULL);
                  g_free (cmd);
                  g_free (bright_path);
                  handled = TRUE;
                }
            }
          g_free (max_path);
        }
      g_dir_close (dir);
      if (handled)
        return;
    }

  if (command_exists ("xbacklight"))
    {
      cmd = g_strdup_printf ("xbacklight -set %d", value);
      spawn_command (cmd);
      g_free (cmd);
      return;
    }

  if (command_exists ("xrandr"))
    {
      name = get_xrandr_output ();
      if (name != NULL)
        {
          if (value >= 100)
            cmd = g_strdup_printf ("xrandr --output %s --brightness 1.0", name);
          else
            cmd = g_strdup_printf ("xrandr --output %s --brightness 0.%02d",
                                   name, value);
          spawn_command (cmd);
          g_free (cmd);
          g_free (name);
        }
    }
}

/* ---------------- audio device listing ----------------
 * pactl preferred, wpctl fallback. Combo id = pactl name or wpctl number. */

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
static GHashTable *
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

static AudioDev *
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

static gchar *
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

static GList *
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

static GList *
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
      /* Skip sink monitors: not real microphones. */
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

/* Parses one "Sinks:" or "Sources:" section of "wpctl status".
 * Entry lines look like: " │  *  56. Description [vol: 1.00]" ("*" = default). */
static GList *
parse_wpctl_section (const gchar *status_output,
                     const gchar *section)
{
  GList  *devs = NULL;
  gchar **lines;
  gboolean in_section = FALSE;
  gint     i;

  if (status_output == NULL)
    return NULL;
  lines = g_strsplit (status_output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *p;
      gboolean     is_default = FALSE;
      gchar       *end = NULL;
      glong        id;
      const gchar *bracket;
      gchar       *desc;
      gchar       *id_str;

      if (!in_section)
        {
          if (strstr (lines[i], section) != NULL)
            in_section = TRUE;
          continue;
        }
      /* Next section or blank line ends this one. */
      if (*lines[i] == '\0'
          || strstr (lines[i], "├─") != NULL
          || strstr (lines[i], "└─") != NULL)
        break;
      /* Skip tree/box-drawing bytes up to '*' or first digit. */
      p = lines[i];
      while (*p != '\0' && *p != '*' && (*p < '0' || *p > '9'))
        p++;
      if (*p == '*')
        {
          is_default = TRUE;
          p++;
          while (*p == ' ')
            p++;
        }
      if (*p < '0' || *p > '9')
        continue;
      id = strtol (p, &end, 10);
      if (end == p || end == NULL || *end != '.')
        continue;
      p = end + 1;
      while (*p == ' ')
        p++;
      bracket = strstr (p, " [vol:");
      desc = (bracket != NULL && bracket > p)
             ? g_strndup (p, (gsize) (bracket - p)) : g_strdup (p);
      g_strstrip (desc);
      id_str = g_strdup_printf ("%ld", id);
      if (*desc == '\0')
        {
          g_free (desc);
          desc = g_strdup (id_str);
        }
      devs = g_list_append (devs, audio_dev_new (id_str, desc, is_default));
      g_free (id_str);
      g_free (desc);
    }
  g_strfreev (lines);
  return devs;
}

static GList *
list_wpctl_sinks (void)
{
  gchar *status = run_command ("wpctl status");
  GList *devs = parse_wpctl_section (status, "Sinks:");

  g_free (status);
  return devs;
}

static GList *
list_wpctl_sources (void)
{
  gchar *status = run_command ("wpctl status");
  GList *devs = parse_wpctl_section (status, "Sources:");

  g_free (status);
  return devs;
}

static GList *
list_sinks (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sinks ();
  if (backend == AUDIO_WPCTL)
    return list_wpctl_sinks ();
  return NULL;
}

static GList *
list_sources (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sources ();
  if (backend == AUDIO_WPCTL)
    return list_wpctl_sources ();
  return NULL;
}

/* Move active streams to target so the switch applies immediately
 * (set-default alone only affects new streams). pactl only. */
static void
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

static void
set_default_sink (QuickSettings *qs,
                  const gchar   *id)
{
  gchar *cmd;

  if (qs->audio_backend == AUDIO_PACTL)
    {
      gchar *quoted = g_shell_quote (id);

      cmd = g_strdup_printf ("pactl set-default-sink %s", quoted);
      spawn_command (cmd);
      g_free (cmd);
      pactl_move_streams ("pactl list short sink-inputs",
                          "pactl move-sink-input", id);
      g_free (quoted);
    }
  else if (qs->audio_backend == AUDIO_WPCTL)
    {
      cmd = g_strdup_printf ("wpctl set-default %s", id);
      spawn_command (cmd);
      g_free (cmd);
    }
}

static void
set_default_source (QuickSettings *qs,
                    const gchar   *id)
{
  gchar *cmd;

  if (qs->audio_backend == AUDIO_PACTL)
    {
      gchar *quoted = g_shell_quote (id);

      cmd = g_strdup_printf ("pactl set-default-source %s", quoted);
      spawn_command (cmd);
      g_free (cmd);
      pactl_move_streams ("pactl list short source-outputs",
                          "pactl move-source-output", id);
      g_free (quoted);
    }
  else if (qs->audio_backend == AUDIO_WPCTL)
    {
      cmd = g_strdup_printf ("wpctl set-default %s", id);
      spawn_command (cmd);
      g_free (cmd);
    }
}

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

/* ---------------- UI ---------------- */

/* Creates a titled group (header label + content grid) inside a vertical
 * box. Returns the content grid for attaching rows. */
static GtkWidget *
make_group (GtkWidget   *box,
            const gchar *title)
{
  GtkWidget *header;
  GtkWidget *grid;
  gchar     *markup;

  markup = g_strdup_printf ("<b>%s</b>", title);
  header = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (header), markup);
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (box), header, FALSE, FALSE, 0);
  g_free (markup);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

  return grid;
}

static GtkWidget *
make_slider_row (GtkWidget    *grid,
                 gint          row,
                 gdouble       max,
                 const gchar  *icon_name,
                 const gchar  *tooltip,
                 GtkWidget   **icon_out,
                 GtkWidget   **scale_out)
{
  GtkWidget *icon;
  GtkWidget *scale;

  icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), icon, 0, row, 1, 1);

  scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, max, 1);
  gtk_scale_set_digits (GTK_SCALE (scale), 0);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  gtk_widget_set_hexpand (scale, TRUE);
  gtk_widget_set_tooltip_text (scale, tooltip);
  gtk_grid_attach (GTK_GRID (grid), scale, 1, row, 1, 1);

  *icon_out = icon;
  *scale_out = scale;
  return scale;
}

static GtkWidget *
make_device_row (GtkWidget   *grid,
                 gint         row,
                 const gchar *icon_name,
                 const gchar *tooltip)
{
  GtkWidget *icon;
  GtkWidget *combo;

  icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), icon, 0, row, 1, 1);

  combo = gtk_combo_box_text_new ();
  gtk_widget_set_hexpand (combo, TRUE);
  gtk_widget_set_tooltip_text (combo, tooltip);
  gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);

  return combo;
}

/* Fill combo with devices, selecting the default one. */
static void
fill_device_combo (GtkWidget *combo,
                   GList     *devs,
                   const gchar *empty_text)
{
  GList *l;

  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
  for (l = devs; l != NULL; l = l->next)
    {
      AudioDev *dev = l->data;

      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                 dev->id, dev->label);
      if (dev->is_default)
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), dev->id);
    }
  if (devs == NULL)
    {
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "none", empty_text);
      gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
      gtk_widget_set_sensitive (combo, FALSE);
    }
  else
    {
      gtk_widget_set_sensitive (combo, TRUE);
    }
}

/* ---------------- battery backend (upower > sysfs) ---------------- */

/* First "upower -e" line mentioning a battery, or NULL (caller frees). */
static gchar *
upower_battery_path (void)
{
  gchar  *output = run_command ("upower -e");
  gchar **lines;
  gchar  *path = NULL;
  gint    i;

  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      if (strstr (lines[i], "BAT") != NULL
          || strstr (lines[i], "battery") != NULL)
        {
          path = g_strdup (g_strstrip (lines[i]));
          break;
        }
    }
  g_strfreev (lines);
  g_free (output);
  return path;
}

static gboolean
get_battery_upower (gint   *percent,
                    gchar **state_out,
                    gchar **time_out)
{
  gchar *path = upower_battery_path ();
  gchar *quoted;
  gchar *cmd;
  gchar *output;
  gchar **lines;
  gchar  *state = NULL;
  gchar  *perc = NULL;
  gchar  *to_empty = NULL;
  gchar  *to_full = NULL;
  gint    i;

  if (path == NULL)
    return FALSE;
  quoted = g_shell_quote (path);
  cmd = g_strdup_printf ("upower -i %s", quoted);
  g_free (quoted);
  g_free (path);
  output = run_command (cmd);
  g_free (cmd);
  if (output == NULL)
    return FALSE;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *line = g_strstrip (lines[i]);

      if (g_str_has_prefix (line, "state:"))
        {
          g_free (state);
          state = g_strdup (g_strstrip ((gchar *) line + strlen ("state:")));
        }
      else if (g_str_has_prefix (line, "percentage:"))
        {
          g_free (perc);
          perc = g_strdup (g_strstrip ((gchar *) line
                                       + strlen ("percentage:")));
        }
      else if (g_str_has_prefix (line, "time to empty:"))
        {
          g_free (to_empty);
          to_empty = g_strdup (g_strstrip ((gchar *) line
                                           + strlen ("time to empty:")));
        }
      else if (g_str_has_prefix (line, "time to full:"))
        {
          g_free (to_full);
          to_full = g_strdup (g_strstrip ((gchar *) line
                                          + strlen ("time to full:")));
        }
    }
  g_strfreev (lines);
  g_free (output);
  *percent = parse_first_percent (perc);
  g_free (perc);
  if (*percent < 0)
    {
      g_free (state);
      g_free (to_empty);
      g_free (to_full);
      return FALSE;
    }
  *state_out = state;
  /* Time remaining depends on the direction. */
  if (state != NULL && g_ascii_strcasecmp (state, "charging") == 0)
    {
      *time_out = to_full;
      g_free (to_empty);
    }
  else
    {
      *time_out = to_empty;
      g_free (to_full);
    }
  return TRUE;
}

/* Fallback without UPower: first power_supply entry with type==Battery. */
static gboolean
get_battery_sysfs (gint   *percent,
                   gchar **state_out,
                   gchar **time_out)
{
  GDir        *dir = g_dir_open ("/sys/class/power_supply", 0, NULL);
  const gchar *entry;
  gboolean     found = FALSE;

  *time_out = NULL;
  if (dir == NULL)
    return FALSE;
  while (!found && (entry = g_dir_read_name (dir)) != NULL)
    {
      gchar *type_path = g_build_filename ("/sys/class/power_supply", entry,
                                           "type", NULL);
      gchar *type = NULL;

      if (g_file_get_contents (type_path, &type, NULL, NULL))
        {
          if (g_strcmp0 (g_strstrip (type), "Battery") == 0)
            {
              gchar *cap_path = g_build_filename ("/sys/class/power_supply",
                                                  entry, "capacity", NULL);
              gchar *st_path = g_build_filename ("/sys/class/power_supply",
                                                 entry, "status", NULL);
              gchar *cap = NULL;

              if (g_file_get_contents (cap_path, &cap, NULL, NULL))
                {
                  *percent = atoi (cap);
                  g_free (cap);
                  found = (*percent >= 0);
                }
              if (found
                  && g_file_get_contents (st_path, state_out, NULL, NULL))
                g_strstrip (*state_out);
              g_free (cap_path);
              g_free (st_path);
            }
          g_free (type);
        }
      g_free (type_path);
    }
  g_dir_close (dir);
  return found;
}

static const gchar *
battery_icon_for_level (gint     percent,
                        gboolean charging)
{
  if (percent >= 100)
    return "battery-full-charged-symbolic";
  if (charging)
    {
      if (percent <= 5)
        return "battery-empty-charging-symbolic";
      if (percent <= 20)
        return "battery-caution-charging-symbolic";
      if (percent <= 40)
        return "battery-low-charging-symbolic";
      if (percent <= 80)
        return "battery-good-charging-symbolic";
      return "battery-full-charging-symbolic";
    }
  if (percent <= 5)
    return "battery-empty-symbolic";
  if (percent <= 20)
    return "battery-caution-symbolic";
  if (percent <= 40)
    return "battery-low-symbolic";
  if (percent <= 80)
    return "battery-good-symbolic";
  return "battery-full-symbolic";
}

/* Refresh icon + label; hides the whole section when no battery exists. */
static void
refresh_battery (QuickSettings *qs)
{
  gint      percent = -1;
  gchar    *state = NULL;
  gchar    *time = NULL;
  gboolean  ok;
  gboolean  charging;
  gchar    *label;
  gchar    *tip;

  if (command_exists ("upower"))
    ok = get_battery_upower (&percent, &state, &time);
  else
    ok = get_battery_sysfs (&percent, &state, &time);
  if (!ok || percent < 0)
    {
      gtk_widget_hide (qs->bat_box);
      g_free (state);
      g_free (time);
      return;
    }
  percent = CLAMP (percent, 0, 100);
  charging = (state != NULL && g_ascii_strcasecmp (state, "charging") == 0);
  gtk_widget_show_all (qs->bat_box);
  gtk_image_set_from_icon_name (GTK_IMAGE (qs->bat_icon),
                                battery_icon_for_level (percent, charging),
                                GTK_ICON_SIZE_LARGE_TOOLBAR);
  if (state != NULL && g_ascii_strcasecmp (state, "fully-charged") == 0)
    label = g_strdup_printf ("%d%% · Fully charged", percent);
  else if (state != NULL && g_ascii_strcasecmp (state, "empty") == 0)
    label = g_strdup_printf ("%d%% · Empty", percent);
  else if (charging)
    label = g_strdup_printf ("%d%% · Charging", percent);
  else
    label = g_strdup_printf ("%d%%", percent);
  gtk_label_set_text (GTK_LABEL (qs->bat_label), label);
  g_free (label);
  if (time != NULL && *time != '\0')
    {
      tip = g_strdup_printf ("%s: %s",
                             charging ? "Time to full" : "Time to empty",
                             time);
      gtk_widget_set_tooltip_text (qs->bat_label, tip);
      g_free (tip);
    }
  else
    {
      gtk_widget_set_tooltip_text (qs->bat_label, NULL);
    }
  g_free (state);
  g_free (time);
}

/* ---------------- connection pages ---------------- */

static void
set_tile_active (GtkWidget *tile,
                 gboolean   on)
{
  GtkStyleContext *ctx = gtk_widget_get_style_context (tile);

  if (on)
    gtk_style_context_add_class (ctx, "qs-tile-active");
  else
    gtk_style_context_remove_class (ctx, "qs-tile-active");
}

static GtkWidget *
make_tile (GtkWidget   *box,
           const gchar *icon_name,
           const gchar *label)
{
  GtkWidget *button;
  GtkWidget *hbox;
  GtkWidget *image;
  GtkWidget *text;

  button = gtk_button_new ();
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 4);
  image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  text = gtk_label_new (label);
  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), text, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_size_request (button, -1, 48);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

/* Generic sub-page: header (back + title + switch), scan button,
 * scrollable list, status line. */
static GtkWidget *
build_device_page (const gchar *title,
                   const gchar *scan_label,
                   GtkWidget  **switch_out,
                   GtkWidget  **scan_out,
                   GtkWidget  **list_out,
                   GtkWidget  **status_out,
                   GtkWidget  **back_out)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *back = gtk_button_new_from_icon_name ("go-previous-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  GtkWidget *title_label = gtk_label_new (NULL);
  GtkWidget *sw = gtk_switch_new ();
  GtkWidget *scan = gtk_button_new_with_label (scan_label);
  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *status = gtk_label_new (NULL);
  gchar     *markup = g_strdup_printf ("<b>%s</b>", title);

  gtk_container_set_border_width (GTK_CONTAINER (page), 12);
  gtk_label_set_markup (GTK_LABEL (title_label), markup);
  g_free (markup);
  gtk_widget_set_hexpand (title_label, TRUE);
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (header), back, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (header), title_label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (header), sw, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (page), header, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (page), scan, FALSE, FALSE, 0);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scroll, -1, 220);
  gtk_container_add (GTK_CONTAINER (scroll), list);
  gtk_box_pack_start (GTK_BOX (page), scroll, TRUE, TRUE, 0);
  gtk_label_set_line_wrap (GTK_LABEL (status), TRUE);
  gtk_widget_set_halign (status, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (page), status, FALSE, FALSE, 0);

  *switch_out = sw;
  *scan_out = scan;
  *list_out = list;
  *status_out = status;
  *back_out = back;
  return page;
}

static void
clear_list_box (GtkWidget *list)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (list));
  GList *l;

  for (l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);
}

static GtkWidget *
make_placeholder_row (const gchar *text)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_container_add (GTK_CONTAINER (row), label);
  gtk_widget_set_sensitive (row, FALSE);
  return row;
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

static void
update_tiles (QuickSettings *qs)
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
  update_tiles (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
}

static void
refresh_slider_values (QuickSettings *qs)
{
  gint volume;
  gint brightness;

  qs->updating = TRUE;

  volume = get_volume_percent ();
  /* pactl allows > 150% (set externally); the slider tops out at 150. */
  volume = (volume > 150) ? 150 : volume;
  if (volume >= 0)
    {
      gtk_range_set_value (GTK_RANGE (qs->vol_scale), volume);
      gtk_image_set_from_icon_name (GTK_IMAGE (qs->vol_icon),
                                    volume_icon_for_level (volume),
                                    GTK_ICON_SIZE_LARGE_TOOLBAR);
      gtk_widget_set_sensitive (qs->vol_scale, TRUE);
      gtk_widget_set_tooltip_text (qs->vol_scale,
                                   volume > 100
                                   ? "Volume (boosted above 100%)"
                                   : "Volume");
    }
  else
    {
      gtk_widget_set_sensitive (qs->vol_scale, FALSE);
      gtk_widget_set_tooltip_text (qs->vol_scale, "Volume unavailable");
    }

  brightness = get_brightness_percent ();
  if (brightness >= 0)
    {
      gtk_range_set_value (GTK_RANGE (qs->bright_scale), brightness);
      gtk_widget_set_sensitive (qs->bright_scale, TRUE);
      gtk_widget_set_tooltip_text (qs->bright_scale, "Screen brightness");
    }
  else
    {
      gtk_widget_set_sensitive (qs->bright_scale, FALSE);
      gtk_widget_set_tooltip_text (qs->bright_scale, "Brightness unavailable");
    }

  qs->updating = FALSE;
}

/* Reload output/input device lists and select the current defaults. */
static void
refresh_audio_devices (QuickSettings *qs)
{
  GList *sinks;
  GList *sources;

  qs->updating = TRUE;

  if (command_exists ("pactl"))
    qs->audio_backend = AUDIO_PACTL;
  else if (command_exists ("wpctl"))
    qs->audio_backend = AUDIO_WPCTL;
  else
    qs->audio_backend = AUDIO_NONE;

  if (qs->audio_backend == AUDIO_NONE)
    {
      fill_device_combo (qs->sink_combo, NULL, "Install pulseaudio-utils");
      fill_device_combo (qs->source_combo, NULL, "Install pulseaudio-utils");
      qs->updating = FALSE;
      return;
    }

  sinks = list_sinks (qs->audio_backend);
  fill_device_combo (qs->sink_combo, sinks, "No output devices");
  g_list_free_full (sinks, audio_dev_free);

  sources = list_sources (qs->audio_backend);
  fill_device_combo (qs->source_combo, sources, "No input devices");
  g_list_free_full (sources, audio_dev_free);

  qs->updating = FALSE;
}

static void
on_volume_changed (GtkRange *range,
                   gpointer  user_data)
{
  QuickSettings *qs = user_data;
  gint           value;

  if (qs->updating)
    return;
  value = (gint) (gtk_range_get_value (range) + 0.5);
  set_volume_percent (value);
  gtk_image_set_from_icon_name (GTK_IMAGE (qs->vol_icon),
                                volume_icon_for_level (value),
                                GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_tooltip_text (qs->vol_scale,
                               value > 100
                               ? "Volume (boosted above 100%)"
                               : "Volume");
}

static void
on_brightness_changed (GtkRange *range,
                       gpointer  user_data)
{
  QuickSettings *qs = user_data;
  gint           value;

  if (qs->updating)
    return;
  value = (gint) (gtk_range_get_value (range) + 0.5);
  set_brightness_percent (value);
}

static void
on_sink_changed (GtkComboBox *combo,
                 gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *id;

  if (qs->updating)
    return;
  id = gtk_combo_box_get_active_id (combo);
  if (id == NULL || *id == '\0' || g_strcmp0 (id, "none") == 0)
    return;
  set_default_sink (qs, id);
}

static void
on_source_changed (GtkComboBox *combo,
                   gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *id;

  if (qs->updating)
    return;
  id = gtk_combo_box_get_active_id (combo);
  if (id == NULL || *id == '\0' || g_strcmp0 (id, "none") == 0)
    return;
  set_default_source (qs, id);
}

static void
position_popup (QuickSettings *qs)
{
  GdkWindow    *button_window;
  GdkDisplay   *display;
  GdkMonitor   *monitor_obj;
  GdkRectangle  monitor;
  GtkAllocation allocation;
  gint          origin_x = 0;
  gint          origin_y = 0;
  gint          popup_width;
  gint          popup_height;
  gint          x;
  gint          y;

  button_window = gtk_widget_get_window (qs->button);
  if (button_window == NULL)
    return;

  gtk_widget_get_allocation (qs->button, &allocation);
  gdk_window_get_origin (button_window, &origin_x, &origin_y);

  display = gtk_widget_get_display (qs->button);
  monitor_obj = gdk_display_get_monitor_at_window (display, button_window);
  if (monitor_obj == NULL)
    monitor_obj = gdk_display_get_monitor (display, 0);
  if (monitor_obj != NULL)
    gdk_monitor_get_workarea (monitor_obj, &monitor);
  else
    monitor = (GdkRectangle) { 0, 0, 1024, 768 };

  gtk_window_get_size (GTK_WINDOW (qs->popup), &popup_width, &popup_height);
  if (popup_width <= 1 || popup_height <= 1)
    {
      popup_width = 300;
      popup_height = 200;
    }

  x = origin_x + allocation.x + allocation.width - popup_width;
  y = origin_y + allocation.y + allocation.height + 6;

  if (x < monitor.x + 6)
    x = monitor.x + 6;
  if (x + popup_width > monitor.x + monitor.width - 6)
    x = monitor.x + monitor.width - popup_width - 6;

  if (y + popup_height > monitor.y + monitor.height - 6)
    y = origin_y + allocation.y - popup_height - 6;
  if (y < monitor.y + 6)
    y = monitor.y + 6;

  gtk_window_move (GTK_WINDOW (qs->popup), x, y);
}

static void
on_button_clicked (GtkButton *button,
                   gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;

  if (gtk_widget_get_visible (qs->popup))
    {
      gtk_widget_hide (qs->popup);
      return;
    }

  refresh_slider_values (qs);
  refresh_audio_devices (qs);
  update_tiles (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
  gtk_widget_show_all (qs->popup);
  refresh_battery (qs); /* after show_all: may hide the battery section */
  while (gtk_events_pending ())
    gtk_main_iteration ();
  position_popup (qs);
  gtk_window_present (GTK_WINDOW (qs->popup));
}

static gboolean
on_size_changed (XfcePanelPlugin *plugin,
                 gint             size,
                 gpointer         user_data)
{
  QuickSettings *qs = user_data;
  gint           icon_size = xfce_panel_plugin_get_icon_size (plugin);

  gtk_image_set_pixel_size (GTK_IMAGE (qs->button_image), MAX (16, icon_size));
  gtk_widget_set_size_request (qs->button, size, size);
  return TRUE;
}

static void
quick_settings_free (XfcePanelPlugin *plugin,
                     gpointer         user_data)
{
  QuickSettings *qs = user_data;

  (void) plugin;

  qs->dead = TRUE;
  gtk_widget_destroy (qs->popup);
  qs_unref (qs);
}

/* ---------------- night light (redshift > gammastep) ---------------- */

static gboolean
night_available (void)
{
  return command_exists ("redshift") || command_exists ("gammastep");
}

/* temp like "6500K", or NULL to reset. */
static void
night_set_temp (const gchar *temp)
{
  const gchar *tool;
  gchar       *cmd;

  if (!night_available ())
    return;
  tool = command_exists ("redshift") ? "redshift" : "gammastep";
  if (temp != NULL)
    cmd = g_strdup_printf ("%s -P -O %s", tool, temp);
  else
    cmd = g_strdup_printf ("%s -x", tool);
  spawn_command (cmd);
  g_free (cmd);
}

static void
on_night_toggled (GtkToggleButton *button,
                  gpointer         user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *temp = g_object_get_data (G_OBJECT (button), "qs-temp");
  GtkWidget     *btns[3] = { qs->night_day, qs->night_aft, qs->night_night };
  gint           i;

  if (qs->updating)
    return;
  if (gtk_toggle_button_get_active (button))
    {
      qs->updating = TRUE;
      for (i = 0; i < 3; i++)
        if (btns[i] != GTK_WIDGET (button))
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btns[i]), FALSE);
      qs->updating = FALSE;
      night_set_temp (temp);
    }
  else
    {
      night_set_temp (NULL);
    }
}

static GtkWidget *
make_night_button (GtkWidget   *box,
                   const gchar *icon_name,
                   const gchar *label,
                   const gchar *temp,
                   gpointer     user_data)
{
  GtkWidget *button = gtk_toggle_button_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                   GTK_ICON_SIZE_MENU);
  GtkWidget *text = gtk_label_new (label);

  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), text, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_tooltip_text (button, temp);
  g_object_set_data (G_OBJECT (button), "qs-temp", (gpointer) temp);
  g_signal_connect (button, "toggled", G_CALLBACK (on_night_toggled),
                    user_data);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

static void
construct_quick_settings (XfcePanelPlugin *plugin)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *vol_grid;
  GtkWidget     *display_grid;
  GtkWidget     *conn_grid;
  GtkWidget     *tile_box;
  GtkWidget     *separator;
  GtkWidget     *wifi_page;
  GtkWidget     *bt_page;
  GtkWidget     *wifi_back;
  GtkWidget     *bt_back;
  GtkWidget     *night_box;

  qs = g_new0 (QuickSettings, 1);
  qs->plugin = plugin;
  qs->refcount = 1;

  /* Accent background for active tiles (Wi-Fi / Bluetooth). */
  {
    static gboolean css_loaded = FALSE;

    if (!css_loaded)
      {
        GtkCssProvider *provider = gtk_css_provider_new ();

        gtk_css_provider_load_from_data (provider,
          ".qs-tile-active { background-color: @theme_selected_bg_color;"
          " color: @theme_selected_fg_color; }", -1, NULL);
        gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
          GTK_STYLE_PROVIDER (provider),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref (provider);
        css_loaded = TRUE;
      }
  }

  xfce_panel_plugin_set_small (plugin, TRUE);

  qs->button = gtk_button_new ();
  qs->button_image = gtk_image_new_from_icon_name ("preferences-system-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  gtk_button_set_relief (GTK_BUTTON (qs->button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (qs->button, "Quick Settings");
  gtk_container_add (GTK_CONTAINER (qs->button), qs->button_image);
  gtk_container_add (GTK_CONTAINER (plugin), qs->button);

  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated (GTK_WINDOW (qs->popup), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (qs->popup), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_keep_above (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_type_hint (GTK_WINDOW (qs->popup), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_widget_set_size_request (qs->popup, 300, -1);

  qs->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (qs->stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_container_add (GTK_CONTAINER (qs->popup), qs->stack);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  gtk_stack_add_named (GTK_STACK (qs->stack), box, "main");

  /* Volume group: level slider + output/input device selectors. */
  vol_grid = make_group (box, "Volume");
  make_slider_row (vol_grid, 0, 150, "audio-volume-high-symbolic", "Volume",
                   &qs->vol_icon, &qs->vol_scale);
  /* Tick mark showing where 100% ends and overamplification begins. */
  gtk_scale_add_mark (GTK_SCALE (qs->vol_scale), 100.0, GTK_POS_BOTTOM, "100%");
  qs->sink_combo = make_device_row (vol_grid, 1, "audio-speakers-symbolic",
                                    "Audio output device");
  qs->source_combo = make_device_row (vol_grid, 2,
                                      "audio-input-microphone-symbolic",
                                      "Audio input device");

  separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start (GTK_BOX (box), separator, FALSE, FALSE, 2);

  /* Display group. */
  display_grid = make_group (box, "Display");
  make_slider_row (display_grid, 0, 100, "display-brightness-symbolic",
                   "Screen brightness",
                   &qs->bright_icon, &qs->bright_scale);

  /* Night light: one-shot color temperature presets. */
  night_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous (GTK_BOX (night_box), TRUE);
  gtk_grid_attach (GTK_GRID (display_grid), night_box, 0, 1, 2, 1);
  qs->night_day = make_night_button (night_box, "weather-clear-symbolic",
                                     "Day", "6500K", qs);
  qs->night_aft = make_night_button (night_box, "weather-few-clouds-symbolic",
                                     "Afternoon", "5500K", qs);
  qs->night_night = make_night_button (night_box,
                                       "weather-clear-night-symbolic",
                                       "Night", "4500K", qs);
  gtk_widget_set_sensitive (night_box, night_available ());

  separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start (GTK_BOX (box), separator, FALSE, FALSE, 2);

  /* Connection group: tiles open the sub-pages. */
  conn_grid = make_group (box, "Connection");
  tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
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
  gtk_stack_add_named (GTK_STACK (qs->stack), wifi_page, "wifi");
  bt_page = build_device_page ("Bluetooth", "Scan for devices",
                               &qs->bt_switch, &qs->bt_scan,
                               &qs->bt_list, &qs->bt_status, &bt_back);
  gtk_stack_add_named (GTK_STACK (qs->stack), bt_page, "bt");

  /* Battery group (hidden when no battery is present). */
  {
    GtkWidget *bat_grid;
    GtkWidget *bat_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

    qs->bat_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start (GTK_BOX (box), qs->bat_box, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (qs->bat_box), bat_sep, FALSE, FALSE, 2);
    bat_grid = make_group (qs->bat_box, "Battery");
    qs->bat_icon = gtk_image_new_from_icon_name ("battery-missing-symbolic",
                                                 GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_halign (qs->bat_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (qs->bat_icon, GTK_ALIGN_CENTER);
    gtk_grid_attach (GTK_GRID (bat_grid), qs->bat_icon, 0, 0, 1, 1);
    qs->bat_label = gtk_label_new ("—");
    gtk_widget_set_halign (qs->bat_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand (qs->bat_label, TRUE);
    gtk_grid_attach (GTK_GRID (bat_grid), qs->bat_label, 1, 0, 1, 1);
  }

  g_signal_connect (qs->vol_scale, "value-changed",
                    G_CALLBACK (on_volume_changed), qs);
  g_signal_connect (qs->bright_scale, "value-changed",
                    G_CALLBACK (on_brightness_changed), qs);
  g_signal_connect (qs->sink_combo, "changed",
                    G_CALLBACK (on_sink_changed), qs);
  g_signal_connect (qs->source_combo, "changed",
                    G_CALLBACK (on_source_changed), qs);
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
  g_signal_connect (qs->button, "clicked", G_CALLBACK (on_button_clicked), qs);
  g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), qs);
  g_signal_connect (plugin, "free-data", G_CALLBACK (quick_settings_free), qs);

  on_size_changed (plugin, xfce_panel_plugin_get_size (plugin), qs);
  refresh_slider_values (qs);
  refresh_audio_devices (qs);
  update_tiles (qs);
  refresh_battery (qs);

  gtk_widget_show_all (GTK_WIDGET (plugin));
}

XFCE_PANEL_PLUGIN_REGISTER (construct_quick_settings)
