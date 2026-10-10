#include "qs-audio.h"
#include "qs-ui.h"
#include "qs-util.h"

static void
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

static const gchar *volume_icon_for_level (gint percent);

static AudioBackend audio_cached_backend = AUDIO_NONE;
static gboolean audio_backend_probed = FALSE;
static gboolean audio_have_amixer = FALSE;

static AudioBackend
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

typedef struct
{
  QuickSettings *qs;
  AudioBackend   backend;
  gboolean       use_amixer;
} AudioVolReq;

static void
audio_volume_done (const gchar *output,
                   const gchar *errout,
                   gboolean     ok,
                   gpointer     user_data)
{
  AudioVolReq   *req = user_data;
  QuickSettings *qs = req->qs;
  gint           value = -1;
  const gchar   *p;

  (void) errout;
  if (!qs->dead && ok && output != NULL)
    {
      if (req->backend == AUDIO_PACTL)
        value = parse_first_percent (output);
      else if (req->backend == AUDIO_WPCTL)
        {
          p = strstr (output, "Volume:");
          if (p != NULL)
            value = (gint) (g_ascii_strtod (p + 7, NULL) * 100.0 + 0.5);
        }
      else if (req->use_amixer)
        value = parse_first_percent (output);
    }
  if (!qs->dead)
    {
      qs->updating = TRUE;
      value = (value > 150) ? 150 : value;
      if (value >= 0)
        {
          gtk_range_set_value (GTK_RANGE (qs->vol_scale), value);
          gtk_image_set_from_icon_name (GTK_IMAGE (qs->vol_icon),
                                        volume_icon_for_level (value),
                                        GTK_ICON_SIZE_LARGE_TOOLBAR);
          gtk_widget_set_sensitive (qs->vol_scale, TRUE);
          gtk_widget_set_tooltip_text (qs->vol_scale,
                                       value > 100
                                       ? "Volume (boosted above 100%)"
                                       : "Volume");
        }
      else
        {
          gtk_widget_set_sensitive (qs->vol_scale, FALSE);
          gtk_widget_set_tooltip_text (qs->vol_scale, "Volume unavailable");
        }
      qs->updating = FALSE;
    }
  qs_unref (qs);
  g_free (req);
}

static void
set_volume_percent (gint value)
{
  gchar *cmd;

  value = CLAMP (value, 0, 150);
  /* Backend já cacheado: sem command_exists/spawn extra na UI.
   * spawn_command é fire-and-forget (não bloqueia). */
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
      /* ALSA caps at 100%, no overamplification. */
      cmd = g_strdup_printf ("amixer -q sset Master %d%%", CLAMP (value, 0, 100));
      break;
    }
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
list_sinks (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sinks ();
  return NULL;
}

static GList *
list_sources (AudioBackend backend)
{
  if (backend == AUDIO_PACTL)
    return list_pactl_sources ();
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
static GtkWidget *
make_device_row (GtkWidget   *grid,
                 gint         row,
                 const gchar *icon_name,
                 const gchar *tooltip)
{
  GtkWidget *icon;
  GtkWidget *combo;

  icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), icon, 0, row, 1, 1);

  combo = gtk_combo_box_text_new ();
  ui_add_class (combo, "qs-combo");
  gtk_widget_set_hexpand (combo, TRUE);
  gtk_widget_set_tooltip_text (combo, tooltip);
  gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);

  return combo;
}

static void
fill_device_combo (GtkWidget   *combo,
                   GList       *devs,
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

/* Reload output/input device lists and select the current defaults.
 * Async: pactl/wpctl leitura roda em worker (sem spawn sync na UI);
 * fill dos combos no idle (main loop) com qs_ref/dead. */
#define AUDIO_DATA_LOADING "qs-audio-loading"

typedef struct
{
  QuickSettings *qs;
  AudioBackend   backend;
  GList         *sinks;
  GList         *sources;
} AudioLoad;

static gboolean
audio_devices_idle (gpointer data)
{
  AudioLoad     *load = data;
  QuickSettings *qs = load->qs;
  GList         *l;

  if (qs->dead)
    {
      g_list_free_full (load->sinks, audio_dev_free);
      g_list_free_full (load->sources, audio_dev_free);
      qs_unref (qs);
      g_free (load);
      return G_SOURCE_REMOVE;
    }
  if (qs->popup != NULL)
    g_object_set_data (G_OBJECT (qs->popup), AUDIO_DATA_LOADING, NULL);

  qs->updating = TRUE;
  fill_device_combo (qs->sink_combo, load->sinks, "No output devices");
  gtk_label_set_text (GTK_LABEL (qs->audio_sub), "No output devices");
  for (l = load->sinks; l != NULL; l = l->next)
    {
      AudioDev *dev = l->data;

      if (dev->is_default)
        {
          gtk_label_set_text (GTK_LABEL (qs->audio_sub), dev->label);
          break;
        }
    }
  fill_device_combo (qs->source_combo, load->sources, "No input devices");
  qs->updating = FALSE;

  g_list_free_full (load->sinks, audio_dev_free);
  g_list_free_full (load->sources, audio_dev_free);
  qs_unref (qs);
  g_free (load);
  return G_SOURCE_REMOVE;
}

static gpointer
audio_devices_thread (gpointer data)
{
  AudioLoad *load = data;

  if (load->backend == AUDIO_PACTL)
    {
      load->sinks = list_sinks (AUDIO_PACTL);
      load->sources = list_sources (AUDIO_PACTL);
    }
  else if (load->backend == AUDIO_WPCTL)
    {
      /* Um único "wpctl status" serve sinks+sources. */
      gchar *status = run_command ("wpctl status");

      load->sinks = parse_wpctl_section (status, "Sinks:");
      load->sources = parse_wpctl_section (status, "Sources:");
      g_free (status);
    }
  g_idle_add (audio_devices_idle, load);
  return NULL;
}

void
audio_refresh_devices (QuickSettings *qs)
{
  AudioBackend backend = audio_get_backend_cached ();
  AudioLoad   *load;

  qs->audio_backend = backend;
  if (backend == AUDIO_NONE)
    {
      qs->updating = TRUE;
      fill_device_combo (qs->sink_combo, NULL, "Install pulseaudio-utils");
      fill_device_combo (qs->source_combo, NULL, "Install pulseaudio-utils");
      gtk_label_set_text (GTK_LABEL (qs->audio_sub), "No audio backend");
      qs->updating = FALSE;
      return;
    }
  if (qs->popup != NULL
      && g_object_get_data (G_OBJECT (qs->popup), AUDIO_DATA_LOADING) != NULL)
    return;
  gtk_label_set_text (GTK_LABEL (qs->audio_sub), "Loading…");
  load = g_new0 (AudioLoad, 1);
  load->qs = qs;
  load->backend = backend;
  qs_ref (qs);
  if (qs->popup != NULL)
    g_object_set_data (G_OBJECT (qs->popup), AUDIO_DATA_LOADING,
                       GUINT_TO_POINTER (1));
  g_thread_unref (g_thread_new ("qs-audio", audio_devices_thread, load));
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
on_audio_tile_clicked (GtkButton *button,
                       gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  audio_refresh_volume (qs);
  audio_refresh_devices (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "audio");
}

static void
on_audio_back_clicked (GtkButton *button,
                       gpointer   user_data)
{
  (void) button;
  show_main_page (user_data);
}

void
audio_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *sound_grid = make_group (box, "Sound");
  GtkWidget *tile_box;
  GtkWidget *audio_tile;
  GtkWidget *audio_page;
  GtkWidget *out_grid;
  GtkWidget *in_grid;
  GtkWidget *audio_back;

  make_slider_row (sound_grid, 0, 150, "audio-volume-high-symbolic", "Volume",
                   &qs->vol_icon, &qs->vol_scale);
  gtk_scale_add_mark (GTK_SCALE (qs->vol_scale), 100.0, GTK_POS_BOTTOM, "100%");
  /* The bottom mark makes this row taller below the trough; keep the
   * icon (and scale) vertically centered so the icon stays with the bar. */
  gtk_widget_set_valign (qs->vol_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (qs->vol_scale, GTK_ALIGN_CENTER);
  /* Tile in a homogeneous grid row like Wi-Fi + Bluetooth (qs-net.c):
   * packed in the main box it would expand vertically with the popup,
   * dwarfing the other tiles; nested here it keeps its natural height. */
  tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (sound_grid), tile_box, 0, 1, 2, 1);
  audio_tile = make_nav_tile (tile_box, "audio-speakers-symbolic",
                              "Audio devices", &qs->audio_sub);
  gtk_label_set_text (GTK_LABEL (qs->audio_sub), "…");
  g_signal_connect (audio_tile, "clicked",
                    G_CALLBACK (on_audio_tile_clicked), qs);

  audio_page = make_sub_page ("Audio devices", &audio_back);
  out_grid = make_group (audio_page, "Output");
  qs->sink_combo = make_device_row (out_grid, 0, "audio-speakers-symbolic",
                                    "Audio output device");
  ui_add_separator (audio_page);
  in_grid = make_group (audio_page, "Input");
  qs->source_combo = make_device_row (in_grid, 0,
                                      "audio-input-microphone-symbolic",
                                      "Audio input device");
  gtk_stack_add_named (GTK_STACK (stack), audio_page, "audio");
  g_signal_connect (audio_back, "clicked",
                    G_CALLBACK (on_audio_back_clicked), qs);

  g_signal_connect (qs->vol_scale, "value-changed",
                    G_CALLBACK (on_volume_changed), qs);
  g_signal_connect (qs->sink_combo, "changed",
                    G_CALLBACK (on_sink_changed), qs);
  g_signal_connect (qs->source_combo, "changed",
                    G_CALLBACK (on_source_changed), qs);
}

/* Refresh slider position/icon. Async de 1 comando do backend cacheado;
 * nunca spawna sync na UI. */
void
audio_refresh_volume (QuickSettings *qs)
{
  AudioBackend backend = audio_get_backend_cached ();
  AudioVolReq *req;
  const gchar *cmd;

  qs->audio_backend = backend;
  if (backend == AUDIO_PACTL)
    cmd = "pactl get-sink-volume @DEFAULT_SINK@";
  else if (backend == AUDIO_WPCTL)
    cmd = "wpctl get-volume @DEFAULT_AUDIO_SINK@";
  else if (audio_have_amixer)
    cmd = "amixer get Master";
  else
    {
      qs->updating = TRUE;
      gtk_widget_set_sensitive (qs->vol_scale, FALSE);
      gtk_widget_set_tooltip_text (qs->vol_scale, "Volume unavailable");
      qs->updating = FALSE;
      return;
    }
  req = g_new0 (AudioVolReq, 1);
  req->qs = qs;
  req->backend = backend;
  req->use_amixer = (backend == AUDIO_NONE && audio_have_amixer);
  qs_ref (qs);
  run_command_async (cmd, audio_volume_done, req);
}
