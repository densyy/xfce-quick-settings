#include "qs-audio.h"
#include "qs-audio-backend.h"
#include "qs-ui.h"
#include "qs-util.h"

typedef struct
{
  QuickSettings *qs;
  AudioBackend   backend;
  gboolean       use_amixer;
  gboolean       is_source;
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
  if (!g_atomic_int_get (&qs->dead) && ok && output != NULL)
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
  if (!g_atomic_int_get (&qs->dead))
    {
      GtkWidget *target_scale = req->is_source ? qs->audio_in_vol_scale : qs->audio_out_vol_scale;
       GtkWidget *target_icon = req->is_source ? qs->audio_in_vol_icon : qs->audio_out_vol_icon;

       if (target_scale == NULL)
        target_scale = qs->vol_scale;
       if (target_icon == NULL)
        target_icon = qs->vol_icon;

       qs->updating = TRUE;
       value = (value > 150) ? 150 : value;
       if (value >= 0)
        {
          gtk_range_set_value (GTK_RANGE (target_scale), value);
           if (target_icon != NULL)
            gtk_image_set_from_icon_name (GTK_IMAGE (target_icon),
                                          volume_icon_for_level (value),
                                          GTK_ICON_SIZE_LARGE_TOOLBAR);
           gtk_widget_set_sensitive (target_scale, TRUE);
           gtk_widget_set_tooltip_text (target_scale,
                                       value > 100
                                       ? "Volume (boosted above 100%)"
                                       : "Volume");
           if (!req->is_source && qs->vol_scale != NULL && qs->vol_scale != target_scale)
            {
              gtk_range_set_value (GTK_RANGE (qs->vol_scale), value);
               if (qs->vol_icon != NULL)
                gtk_image_set_from_icon_name (GTK_IMAGE (qs->vol_icon),
                                              volume_icon_for_level (value),
                                              GTK_ICON_SIZE_LARGE_TOOLBAR);
            }
        }
      else
        {
          gtk_widget_set_sensitive (target_scale, FALSE);
           gtk_widget_set_tooltip_text (target_scale, "Volume unavailable");
        }
      qs->updating = FALSE;
     }
  qs_unref (qs);
  g_free (req);
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
      if (*lines[i] == '\0'
          || strstr (lines[i], "├─") != NULL
          || strstr (lines[i], "└─") != NULL)
        break;
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

  if (g_atomic_int_get (&qs->dead))
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
  {
    GThread *t = g_thread_try_new ("qs-audio", audio_devices_thread, load, NULL);

    if (t == NULL)
      {
        qs_unref (qs);
        g_free (load);
      }
    else
      g_thread_unref (t);
  }
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
void audio_refresh_input_volume (QuickSettings *qs);
}

void audio_refresh_input_volume (QuickSettings *qs);
static void
on_sink_volume_changed (GtkRange *range,
                       gpointer  user_data)
{
  QuickSettings *qs = user_data;
  if (qs->updating)
    return;
  {
    gint value = (gint) (gtk_range_get_value (range) + 0.5);
    set_volume_percent (value);
    if (qs->audio_out_vol_icon != NULL)
      gtk_image_set_from_icon_name (GTK_IMAGE (qs->audio_out_vol_icon),
                                    volume_icon_for_level (value),
                                    GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (qs->audio_out_vol_scale != NULL)
      gtk_widget_set_tooltip_text (qs->audio_out_vol_scale,
                                   value > 100
                                   ? "Volume (boosted above 100%)"
                                   : "Volume");
  }
}

static void
on_source_volume_changed (GtkRange *range,
                          gpointer  user_data)
{
  QuickSettings *qs = user_data;
  if (qs->updating)
    return;
  {
    gint value = (gint) (gtk_range_get_value (range) + 0.5);
    set_source_volume_percent (value);
    if (qs->audio_in_vol_icon != NULL)
      gtk_image_set_from_icon_name (GTK_IMAGE (qs->audio_in_vol_icon),
                                    volume_icon_for_level (value),
                                    GTK_ICON_SIZE_LARGE_TOOLBAR);
    if (qs->audio_in_vol_scale != NULL)
      gtk_widget_set_tooltip_text (qs->audio_in_vol_scale, "Input volume");
  }
}

static void
on_audio_tile_clicked (GtkButton *button,
                       gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  audio_refresh_volume (qs);
  audio_refresh_input_volume (qs);

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
  make_slider_row (out_grid, 0, 150, "audio-volume-high-symbolic", "Output volume",
 & qs->audio_out_vol_icon, & qs->audio_out_vol_scale); 
  gtk_scale_add_mark (GTK_SCALE (qs->audio_out_vol_scale), 100.0, GTK_POS_BOTTOM, "100%"); 
  qs->sink_combo = make_device_row (out_grid, 1, "audio-speakers-symbolic",
                                  "Audio output device"); 
  ui_add_separator (audio_page);
  in_grid = make_group (audio_page, "Input");
  make_slider_row (in_grid, 0, 150, "audio-input-microphone-symbolic", "Input volume",
 & qs->audio_in_vol_icon, & qs->audio_in_vol_scale); 
  gtk_scale_add_mark (GTK_SCALE (qs->audio_in_vol_scale), 100.0, GTK_POS_BOTTOM, "100%"); 
  qs->source_combo = make_device_row (in_grid, 1,
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
  g_signal_connect (qs->audio_out_vol_scale, "value-changed",
                    G_CALLBACK (on_sink_volume_changed), qs);
  g_signal_connect (qs->audio_in_vol_scale, "value-changed",
                    G_CALLBACK (on_source_volume_changed), qs);
}

/* Refresh slider position/icon. Async de 1 comando do backend cacheado;
 * nunca spawna sync na UI. */
void
audio_refresh_input_volume (QuickSettings *qs)
{
  AudioBackend backend = audio_get_backend_cached ();
  AudioVolReq *req;
  const gchar *cmd;

  qs->audio_backend = backend;
  if (backend == AUDIO_PACTL)
    cmd = "pactl get-source-volume @DEFAULT_SOURCE@";
  else if (backend == AUDIO_WPCTL)
    cmd = "wpctl get-volume @DEFAULT_SOURCE@";
  else if (audio_have_amixer_avail ())
    cmd = "amixer get Capture";
  else
    {
      if (qs->audio_in_vol_scale != NULL)
        {
          qs->updating = TRUE;
          gtk_widget_set_sensitive (qs->audio_in_vol_scale, FALSE);
          gtk_widget_set_tooltip_text (qs->audio_in_vol_scale, "Volume unavailable");
          qs->updating = FALSE;
        }
      return;
    }
  req = g_new0 (AudioVolReq, 1);
  req->qs = qs;
  req->backend = backend;
  req->use_amixer = (backend == AUDIO_NONE && audio_have_amixer_avail ());
  req->is_source = TRUE;
  qs_ref (qs);
  run_command_async (cmd, audio_volume_done, req);
}

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
  else if (audio_have_amixer_avail ())
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
  req->use_amixer = (backend == AUDIO_NONE && audio_have_amixer_avail ());
  qs_ref (qs);
  run_command_async (cmd, audio_volume_done, req);
}
