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
  gboolean         updating; /* TRUE while loading current values */
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

  value = CLAMP (value, 0, 100);
  if (command_exists ("pactl"))
    cmd = g_strdup_printf ("pactl set-sink-volume @DEFAULT_SINK@ %d%%", value);
  else if (command_exists ("wpctl"))
    cmd = g_strdup_printf ("wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", value);
  else if (command_exists ("amixer"))
    cmd = g_strdup_printf ("amixer -q sset Master %d%%", value);
  else
    return;
  spawn_command (cmd);
  g_free (cmd);
}

static const gchar *
volume_icon_for_level (gint percent)
{
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

/* ---------------- UI ---------------- */

static GtkWidget *
make_slider_row (GtkWidget    *grid,
                 gint          row,
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

  scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
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

static void
refresh_slider_values (QuickSettings *qs)
{
  gint volume;
  gint brightness;

  qs->updating = TRUE;

  volume = get_volume_percent ();
  if (volume >= 0)
    {
      gtk_range_set_value (GTK_RANGE (qs->vol_scale), volume);
      gtk_image_set_from_icon_name (GTK_IMAGE (qs->vol_icon),
                                    volume_icon_for_level (volume),
                                    GTK_ICON_SIZE_LARGE_TOOLBAR);
      gtk_widget_set_sensitive (qs->vol_scale, TRUE);
      gtk_widget_set_tooltip_text (qs->vol_scale, "Volume");
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
  gtk_widget_show_all (qs->popup);
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

  g_free (qs);
}

static void
construct_quick_settings (XfcePanelPlugin *plugin)
{
  QuickSettings *qs;
  GtkWidget     *grid;
  GtkWidget     *separator;

  qs = g_new0 (QuickSettings, 1);
  qs->plugin = plugin;

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

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
  gtk_container_add (GTK_CONTAINER (qs->popup), grid);

  make_slider_row (grid, 0, "audio-volume-high-symbolic", "Volume",
                   &qs->vol_icon, &qs->vol_scale);
  make_slider_row (grid, 1, "display-brightness-symbolic", "Screen brightness",
                   &qs->bright_icon, &qs->bright_scale);

  separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_grid_attach (GTK_GRID (grid), separator, 0, 2, 2, 1);

  qs->sink_combo = make_device_row (grid, 3, "audio-speakers-symbolic",
                                    "Audio output device");
  qs->source_combo = make_device_row (grid, 4, "audio-input-microphone-symbolic",
                                      "Audio input device");

  g_signal_connect (qs->vol_scale, "value-changed",
                    G_CALLBACK (on_volume_changed), qs);
  g_signal_connect (qs->bright_scale, "value-changed",
                    G_CALLBACK (on_brightness_changed), qs);
  g_signal_connect (qs->sink_combo, "changed",
                    G_CALLBACK (on_sink_changed), qs);
  g_signal_connect (qs->source_combo, "changed",
                    G_CALLBACK (on_source_changed), qs);
  g_signal_connect (qs->button, "clicked", G_CALLBACK (on_button_clicked), qs);
  g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), qs);
  g_signal_connect (plugin, "free-data", G_CALLBACK (quick_settings_free), qs);

  on_size_changed (plugin, xfce_panel_plugin_get_size (plugin), qs);
  refresh_slider_values (qs);
  refresh_audio_devices (qs);

  gtk_widget_show_all (GTK_WIDGET (plugin));
}

XFCE_PANEL_PLUGIN_REGISTER (construct_quick_settings)
