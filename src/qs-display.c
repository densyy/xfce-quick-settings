#include "qs-display.h"
#include "qs-ui.h"
#include "qs-util.h"

/* ---------------- brightness backend ----------------
 * brightnessctl > sysfs > xbacklight > xrandr
 * Piso anti-tela-preta: 0% apaga backlight/gamma (irrecuperável sem
 * ver a tela). Setter e slider nunca descem abaixo de MIN.
 * xrandr é gamma software: Fn não recupera, então tem piso próprio
 * maior e nunca é usado quando há backlight real. */
#define BRIGHTNESS_MIN 5
#define BRIGHTNESS_MIN_XRANDR 30

/* Melhor device em /sys/class/backlight, ou NULL (caller frees).
 * Descarta max<=0; prefere driver nativo (intel/amdgpu/nv) a acpi_video*. */
static gchar *
pick_backlight_device (void)
{
  GDir *dir = g_dir_open ("/sys/class/backlight", 0, NULL);
  const gchar *e;
  gchar *best = NULL;
  gint best_score = -1;

  if (dir == NULL)
    return NULL;
  while ((e = g_dir_read_name (dir)) != NULL)
    {
      gchar *mp = g_build_filename ("/sys/class/backlight", e,
                                    "max_brightness", NULL);
      gchar *c = NULL;
      gint maxv = 0;
      gint score;

      if (g_file_get_contents (mp, &c, NULL, NULL))
        {
          maxv = atoi (c);
          g_free (c);
        }
      g_free (mp);
      if (maxv <= 0)
        continue;
      if (g_str_has_prefix (e, "intel_backlight")
          || strstr (e, "amdgpu") != NULL
          || strstr (e, "nvidia") != NULL
          || g_str_has_prefix (e, "nv_"))
        score = 2;
      else if (g_str_has_prefix (e, "acpi_video"))
        score = 0;
      else
        score = 1;
      if (score > best_score)
        {
          g_free (best);
          best = g_strdup (e);
          best_score = score;
        }
    }
  g_dir_close (dir);
  return best;
}

static gint
get_brightness_percent (void)
{
  gchar       *output;
  gint         value = -1;
  gchar       *dev;
  gchar       *bright_path;
  gchar       *max_path;
  gchar       *contents = NULL;
  gint         brightness;
  gint         max_brightness;
  const gchar *p;

  if (command_exists ("brightnessctl"))
    {
      /* -m é multi-linha quando há 2 devices; parse_first_percent pega
       * o primeiro "NN%" (primeiro device backlight). Tenta com -c,
       * depois sem -c (brightnessctl antigo sem flag -c). */
      output = run_command ("brightnessctl -c backlight -m");
      if (output != NULL)
        {
          value = parse_first_percent (output);
          g_free (output);
          if (value >= 0)
            return CLAMP (value, 0, 100);
        }
      output = run_command ("brightnessctl -m");
      if (output != NULL)
        {
          value = parse_first_percent (output);
          g_free (output);
          if (value >= 0)
            return CLAMP (value, 0, 100);
        }
    }

  dev = pick_backlight_device ();
  if (dev != NULL)
    {
      bright_path = g_build_filename ("/sys/class/backlight", dev,
                                       "brightness", NULL);
      max_path = g_build_filename ("/sys/class/backlight", dev,
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
      g_free (dev);
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
  gchar       *dev;
  gchar       *bright_path;
  gchar       *max_path;
  gchar       *contents = NULL;
  gint         max_brightness;
  gboolean     handled = FALSE;
  gboolean     has_backlight = FALSE;
  gchar       *name;

  value = CLAMP (value, BRIGHTNESS_MIN, 100);

  if (command_exists ("brightnessctl"))
    {
      /* Mesmo comando que o script de teste (provado na sua máquina),
       * mas mirando o mesmo device do getter (intel > acpi_video*).
       * Async como o botão de volume: não trava o painel.
       * Caminho absoluto: o PATH do painel pode ser mínimo. */
      gchar *target = pick_backlight_device ();
      gchar *bin = g_find_program_in_path ("brightnessctl");

      if (target != NULL && bin != NULL)
        {
          /* Nomes de device são [A-Za-z0-9_+-]: sem quote, sem
           * risco de parse do g_spawn_command_line_async. */
          cmd = g_strdup_printf ("%s -d %s set %d%%", bin, target, value);
          g_free (target);
          g_free (bin);
        }
      else
        {
          g_free (target);
          if (bin != NULL)
            {
              cmd = g_strdup_printf ("%s set %d%%", bin, value);
              g_free (bin);
            }
          else
            {
              cmd = g_strdup_printf ("brightnessctl set %d%%", value);
            }
        }
      spawn_command (cmd);
      g_free (cmd);
      return;
    }

  dev = pick_backlight_device ();
  if (dev != NULL)
    {
      has_backlight = TRUE;
      max_path = g_build_filename ("/sys/class/backlight", dev,
                                    "max_brightness", NULL);
      if (g_file_get_contents (max_path, &contents, NULL, NULL))
        {
          max_brightness = atoi (contents);
          g_free (contents);
          contents = NULL;
          if (max_brightness > 0)
            {
              bright_path = g_build_filename ("/sys/class/backlight",
                                               dev, "brightness", NULL);
              cmd = g_strdup_printf ("%d", value * max_brightness / 100);
              /* Direct sysfs write needs root: only claim handled
               * on success, otherwise fall through to
               * xbacklight/xrandr instead of silently doing nothing. */
              if (g_file_set_contents (bright_path, cmd, -1, NULL))
                handled = TRUE;
              g_free (cmd);
              g_free (bright_path);
            }
        }
      g_free (max_path);
      g_free (dev);
    }
  if (handled)
    return;

  if (command_exists ("xbacklight"))
    {
      cmd = g_strdup_printf ("xbacklight -set %d", value);
      spawn_command (cmd);
      g_free (cmd);
      return;
    }

  if (!has_backlight && command_exists ("xrandr"))
    {
      /* Sem backlight real (desktop): gamma é a única opção.
       * Piso alto: 0.05 parece apagado e assusta. */
      value = MAX (value, BRIGHTNESS_MIN_XRANDR);
      name = get_xrandr_output ();
      if (name != NULL)
        {
          gchar *qname = g_shell_quote (name);

          cmd = g_strdup_printf ("xrandr --output %s --brightness %.2f",
                                 qname, value / 100.0);
          g_free (qname);
          spawn_command (cmd);
          g_free (cmd);
          g_free (name);
        }
    }
}
static void
on_brightness_changed (GtkRange *range,
                       gpointer  user_data)
{
  QuickSettings *qs = user_data;
  gint           value;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (range)))
    return;
  value = (gint) (gtk_range_get_value (range) + 0.5);
  set_brightness_percent (value);
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
night_update_sub (QuickSettings *qs)
{
  const gchar *label = "Off";

  if (qs->night_sub == NULL)
    return;
  if ((qs->night_day != NULL
       && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_day))))
    label = "Day";
  else if ((qs->night_aft != NULL
            && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_aft))))
    label = "Afternoon";
  else if ((qs->night_night != NULL
            && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_night))))
    label = "Night";
  else if ((qs->night_dawn != NULL
            && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (qs->night_dawn))))
    label = "Dawn";
  gtk_label_set_text (GTK_LABEL (qs->night_sub), label);
}

static void
on_night_toggled (GtkToggleButton *button,
                  gpointer         user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *temp = g_object_get_data (G_OBJECT (button), "qs-temp");
  GtkWidget     *btns[4] = { qs->night_day, qs->night_aft,
                             qs->night_night, qs->night_dawn };
  gint           i;

  if (qs->updating)
    return;
  if (gtk_toggle_button_get_active (button))
    {
      qs->updating = TRUE;
      for (i = 0; i < 4; i++)
        if (btns[i] != GTK_WIDGET (button))
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btns[i]), FALSE);
      qs->updating = FALSE;
      night_set_temp (temp);
    }
  else
    {
      night_set_temp (NULL);
    }
  night_update_sub (qs);
}

static GtkWidget *
make_night_button (GtkWidget   *box,
                   const gchar *icon_name,
                   const gchar *label,
                   const gchar *temp,
                   gpointer     user_data)
{
  GtkWidget *button = gtk_toggle_button_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                   GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *sub = gtk_label_new (temp);

  /* Same list pattern as the Power page buttons; qs-tile keeps the
   * :checked highlight for the active preset, qs-compact collapses the
   * vertical padding so 4 presets fit in less space. */
  ui_add_class (button, "qs-tile");
  ui_add_class (button, "qs-compact");
  gtk_image_set_pixel_size (GTK_IMAGE (image), QS_ROW_ICON_PX);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), QS_TIGHT_GAP);
  ui_add_class (text, "qs-title");
  gtk_widget_set_halign (text, GTK_ALIGN_START);
  ui_add_class (sub, "qs-sub");
  gtk_widget_set_halign (sub, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (vbox), text, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), sub, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), vbox, TRUE, TRUE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_halign (hbox, GTK_ALIGN_START);
  g_object_set_data (G_OBJECT (button), "qs-temp", (gpointer) temp);
  g_signal_connect (button, "toggled", G_CALLBACK (on_night_toggled),
                    user_data);
  gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
  return button;
}

/* ---------------- external monitors (xrandr) ----------------
 * Query:  xrandr --query            (connected outputs + their modes)
 * Mode:   xrandr --output MON --mode MODE
 * Scale:  xrandr --output MON --scale FxF
 * --scale chosen over --scale-from: --scale takes the UI factor directly
 * (1.5 -> "1.5x1.5"); --scale-from would need per-monitor pixel math to
 * express the same factor. xrandr accepts any positive factor, so every
 * entry in SCALE_FACTORS is valid (nothing to filter).
 * Without xrandr every combo shows "unavailable" and stays insensitive.
 * Queries run via run_command_async; applies via spawn_command: the
 * panel never blocks. */

static const gchar *SCALE_FACTORS[] = { "1", "1.25", "1.5", "1.75", "2" };

/* Valid output name ([A-Za-z0-9_.-], xrandr never emits anything else).
 * Rejected names are skipped at parse time and never applied. */
static gboolean
valid_monitor (const gchar *s)
{
  const gchar *p;

  if (s == NULL || *s == '\0' || strlen (s) > 64)
    return FALSE;
  for (p = s; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_' && *p != '.')
      return FALSE;
  return TRUE;
}

/* Valid mode ("1920x1080"): digits, one 'x', digits. */
static gboolean
valid_mode (const gchar *s)
{
  const gchar *x;
  const gchar *p;

  if (s == NULL || !g_ascii_isdigit (*s))
    return FALSE;
  x = strchr (s, 'x');
  if (x == NULL || x == s || x[1] == '\0')
    return FALSE;
  for (p = s; p < x; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  for (p = x + 1; *p != '\0'; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return TRUE;
}

static gboolean
valid_scale (const gchar *s)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    if (g_strcmp0 (s, SCALE_FACTORS[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Connected outputs from "xrandr --query": header lines look like
 * "eDP-1 connected [primary] 1600x900+0+0 (...)". The "connected" token
 * must match exactly: "disconnected" lines carry no such token, so token
 * comparison (not strstr) tells them apart. Returns GList of gchar*. */
static GList *
parse_monitors (const gchar *output)
{
  GList  *mons = NULL;
  gchar **lines;
  gint    i;

  if (output == NULL)
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar  *s = g_strdup (lines[i]);
      gchar **tok;
      gint    t;
      gboolean connected = FALSE;

      g_strstrip (s);
      if (*s == '\0')
        {
          g_free (s);
          continue;
        }
      tok = g_strsplit_set (s, " \t", -1);
      for (t = 1; tok[t] != NULL; t++)
        if (g_strcmp0 (tok[t], "connected") == 0)
          connected = TRUE;
      if (connected && tok[0] != NULL && valid_monitor (tok[0]))
        mons = g_list_append (mons, g_strdup (tok[0]));
      g_strfreev (tok);
      g_free (s);
    }
  g_strfreev (lines);
  return mons;
}

/* Modes of one monitor: indented lines after its "connected" header,
 * e.g. '   1920x1080     60.00 +  60.00'. current_out (caller frees)
 * gets the mode whose line carries '*'. Returns GList of gchar*. */
static GList *
parse_modes (const gchar *output,
             const gchar *monitor,
             gchar      **current_out)
{
  GList  *modes = NULL;
  gchar **lines;
  gint    i;
  gboolean in_block = FALSE;

  if (current_out != NULL)
    *current_out = NULL;
  if (output == NULL || !valid_monitor (monitor))
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *line = lines[i];

      if (*line == '\0')
        continue;
      if (*line == ' ' || *line == '\t')
        {
          gchar *s;
          gchar *end;

          if (!in_block)
            continue;
          s = g_strdup (line);
          g_strstrip (s);
          end = strpbrk (s, " \t");
          if (end != NULL)
            *end = '\0';
          if (valid_mode (s))
            {
              modes = g_list_append (modes, g_strdup (s));
              if (current_out != NULL && *current_out == NULL
                  && strchr (line, '*') != NULL)
                *current_out = g_strdup (s);
            }
          g_free (s);
        }
      else
        {
          gchar  *s = g_strdup (line);
          gchar **tok;
          gint    t;
          gboolean connected = FALSE;

          g_strstrip (s);
          tok = g_strsplit_set (s, " \t", -1);
          for (t = 1; tok[t] != NULL; t++)
            if (g_strcmp0 (tok[t], "connected") == 0)
              connected = TRUE;
          in_block = (connected && tok[0] != NULL
                      && g_strcmp0 (tok[0], monitor) == 0);
          g_strfreev (tok);
          g_free (s);
        }
    }
  g_strfreev (lines);
  return modes;
}

/* Header line ("MON connected ..."/"MON disconnected ...") of one output,
 * or NULL (caller frees). Exact "connected" token match, same as
 * parse_monitors: "disconnected" lines carry no such token. */
static gchar *
monitor_header (const gchar *output,
                const gchar *monitor)
{
  gchar  **lines;
  gint     i;
  gchar   *found = NULL;

  if (output == NULL || !valid_monitor (monitor))
    return NULL;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL && found == NULL; i++)
    {
      const gchar *line = lines[i];

      if (*line == ' ' || *line == '\t' || *line == '\0')
        continue;
      {
        gchar  *s = g_strdup (line);
        gchar **tok;

        g_strstrip (s);
        tok = g_strsplit_set (s, " \t", -1);
        if (tok[0] != NULL && g_strcmp0 (tok[0], monitor) == 0)
          found = g_strdup (lines[i]);
        g_strfreev (tok);
        g_free (s);
      }
    }
  g_strfreev (lines);
  return found;
}

/* Geometry token ("1920x1080+0+0") in a header means the output is
 * actively driving pixels. After "--off" the header keeps "connected"
 * but loses the geometry (modes stay listed, '*' disappears), so the
 * header alone tells on from off. */
static gboolean
header_has_geometry (const gchar *header)
{
  gchar    **tok;
  gint       t;
  gboolean   active = FALSE;

  if (header == NULL)
    return FALSE;
  tok = g_strsplit_set (header, " \t", -1);
  for (t = 0; tok[t] != NULL; t++)
    {
      if (g_ascii_isdigit (tok[t][0])
          && strchr (tok[t], 'x') != NULL
          && strchr (tok[t], '+') != NULL)
        {
          active = TRUE;
          break;
        }
    }
  g_strfreev (tok);
  return active;
}

/* TRUE when the monitor is currently on: header carries geometry, or its
 * mode block still marks a current ('*') mode. Either signal suffices;
 * both are gone only once the output is really off/disconnected. */
static gboolean
monitor_is_active (const gchar *output,
                   const gchar *monitor)
{
  gchar    *header;
  gboolean  active;
  GList    *modes;
  gchar    *cur = NULL;

  if (output == NULL || !valid_monitor (monitor))
    return FALSE;
  header = monitor_header (output, monitor);
  active = header_has_geometry (header);
  g_free (header);
  if (active)
    return TRUE;
  modes = parse_modes (output, monitor, &cur);
  g_list_free_full (modes, g_free);
  active = (cur != NULL);
  g_free (cur);
  return active;
}

/* Connected vs active output counts from one "xrandr --query" dump.
 * NULL out-params are skipped. */
static void
count_displays (const gchar *output,
                gint        *connected_out,
                gint        *active_out)
{
  gchar **lines;
  gint    i;

  if (connected_out != NULL)
    *connected_out = 0;
  if (active_out != NULL)
    *active_out = 0;
  if (output == NULL)
    return;
  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *line = lines[i];

      if (*line == ' ' || *line == '\t' || *line == '\0')
        continue;
      {
        gchar    *s = g_strdup (line);
        gchar   **tok;
        gint      t;
        gboolean  connected = FALSE;

        g_strstrip (s);
        tok = g_strsplit_set (s, " \t", -1);
        for (t = 1; tok[t] != NULL; t++)
          if (g_strcmp0 (tok[t], "connected") == 0)
            connected = TRUE;
        if (connected)
          {
            if (connected_out != NULL)
              (*connected_out)++;
            if (active_out != NULL && header_has_geometry (line))
              (*active_out)++;
          }
        g_strfreev (tok);
        g_free (s);
      }
    }
  g_strfreev (lines);
}

/* Syncs the "Monitor on/off" switch to the selected monitor. Caller holds
 * qs->updating (programmatic set must not fire the apply handler).
 * Single-display guard: turning off the only active output would black
 * out the session, so the switch goes insensitive with a tooltip + status
 * of "Cannot disable the only display". An output that is already off
 * stays sensitive so it can always be turned back on. */
static void
display_update_power_switch (QuickSettings *qs,
                             const gchar   *sel,
                             const gchar   *output)
{
  gboolean     on = FALSE;
  gint         n_active = 0;
  gboolean     only = FALSE;
  const gchar *cur_status;

  if (qs->mon_switch == NULL)
    return;
  if (sel != NULL && output != NULL)
    {
      on = monitor_is_active (output, sel);
      count_displays (output, NULL, &n_active);
      only = (on && n_active <= 1);
    }
  gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), on);
  gtk_widget_set_sensitive (qs->mon_switch,
                            (sel != NULL && output != NULL && !only));
  if (sel == NULL || output == NULL)
    {
      gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
      return;
    }
  if (only)
    {
      gtk_widget_set_tooltip_text (qs->mon_switch,
                                   "Cannot disable the only display");
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                          "Cannot disable the only display.");
    }
  else
    {
      gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
      /* Drop a stale guard message when the selection now allows off. */
      cur_status = gtk_label_get_text (GTK_LABEL (qs->display_status));
      if (g_strcmp0 (cur_status, "Cannot disable the only display.") == 0)
        gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
    }
}

/* Icon + combo row on a group grid (same pattern as the audio devices). */
static GtkWidget *
make_combo_row (GtkWidget   *grid,
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

/* Fill a text combo (id == label); empty list -> single insensitive row. */
static void
fill_combo (GtkWidget   *combo,
            GList       *items,
            const gchar *active,
            const gchar *empty_text)
{
  GList *l;

  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
  for (l = items; l != NULL; l = l->next)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                               (const gchar *) l->data,
                               (const gchar *) l->data);
  if (items == NULL)
    {
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                 "none", empty_text);
      gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
      gtk_widget_set_sensitive (combo, FALSE);
      return;
    }
  gtk_widget_set_sensitive (combo, TRUE);
  if (active != NULL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), active);
  if (gtk_combo_box_get_active (GTK_COMBO_BOX (combo)) < 0)
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
}

static void
display_update_sub (QuickSettings *qs,
                    const gchar   *mon,
                    const gchar   *mode)
{
  gchar *text;

  if (mon == NULL)
    text = g_strdup ("No displays");
  else if (mode != NULL)
    text = g_strdup_printf ("%s · %s", mon, mode);
  else
    text = g_strdup (mon);
  gtk_label_set_text (GTK_LABEL (qs->display_sub), text);
  g_free (text);
}

/* Everything "unavailable": no xrandr on PATH (or query failed hard at
 * tile-refresh time). Mirrors the audio "No audio backend" pattern. */
static void
display_unavailable (QuickSettings *qs)
{
  qs->updating = TRUE;
  fill_combo (qs->mon_combo, NULL, NULL, "unavailable");
  fill_combo (qs->mode_combo, NULL, NULL, "unavailable");
  fill_combo (qs->scale_combo, NULL, NULL, "unavailable");
  if (qs->mon_switch != NULL)
    {
      gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), FALSE);
      gtk_widget_set_sensitive (qs->mon_switch, FALSE);
      gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
    }
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                       "xrandr not found (unavailable).");
  gtk_label_set_text (GTK_LABEL (qs->display_sub), "unavailable");
  gtk_widget_set_sensitive (qs->display_tile, FALSE);
  qs->updating = FALSE;
}

static void
display_query_done (const gchar *output,
                    const gchar *errout,
                    gboolean     ok,
                    gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gchar         *prev;
  GList         *mons = NULL;
  GList         *l;
  gchar         *sel = NULL;
  GList         *modes = NULL;
  gchar         *cur = NULL;
  const gchar   *shown_mode;
  gsize          i;

  (void) errout;
  if (qs->dead)
    {
      qs_unref (qs);
      return;
    }
  qs->display_loading = FALSE;
  qs->updating = TRUE;

  if (!ok || output == NULL || *output == '\0')
    {
      fill_combo (qs->mon_combo, NULL, NULL, "No displays");
      fill_combo (qs->mode_combo, NULL, NULL, "No displays");
      fill_combo (qs->scale_combo, NULL, NULL, "No displays");
      if (qs->mon_switch != NULL)
        {
          gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), FALSE);
          gtk_widget_set_sensitive (qs->mon_switch, FALSE);
          gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
        }
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                           "Could not query displays.");
      display_update_sub (qs, NULL, NULL);
      qs->updating = FALSE;
      qs_unref (qs);
      return;
    }

  g_free (qs->display_last);
  qs->display_last = g_strdup (output);

  /* Keep the user's current selection when still connected. */
  prev = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo)));
  if (prev != NULL
      && (g_strcmp0 (prev, "none") == 0 || !valid_monitor (prev)))
    {
      g_free (prev);
      prev = NULL;
    }
  mons = parse_monitors (output);
  for (l = mons; l != NULL; l = l->next)
    if (prev != NULL && g_strcmp0 ((const gchar *) l->data, prev) == 0)
      {
        sel = g_strdup ((const gchar *) l->data);
        break;
      }
  g_free (prev);
  if (sel == NULL && mons != NULL)
    sel = g_strdup ((const gchar *) mons->data);
  fill_combo (qs->mon_combo, mons, sel, "No displays");
  g_list_free_full (mons, g_free);

  if (sel != NULL)
    {
      modes = parse_modes (output, sel, &cur);
      shown_mode = (cur != NULL) ? cur
                   : (modes != NULL ? (const gchar *) modes->data : NULL);
      fill_combo (qs->mode_combo, modes, shown_mode, "No modes");

      gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (qs->scale_combo));
      for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
        {
          gchar *label = g_strdup_printf ("%sx", SCALE_FACTORS[i]);

          gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->scale_combo),
                                     SCALE_FACTORS[i], label);
          g_free (label);
        }
      gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->scale_combo), "1");
      gtk_widget_set_sensitive (qs->scale_combo, TRUE);

      gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
      if (monitor_is_active (output, sel))
        display_update_sub (qs, sel, shown_mode);
      else
        display_update_sub (qs, sel, "Off");
      display_update_power_switch (qs, sel, output);
      g_list_free_full (modes, g_free);
      g_free (cur);
    }
  else
    {
      fill_combo (qs->mode_combo, NULL, NULL, "No displays");
      fill_combo (qs->scale_combo, NULL, NULL, "No displays");
      if (qs->mon_switch != NULL)
        {
          gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), FALSE);
          gtk_widget_set_sensitive (qs->mon_switch, FALSE);
          gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
        }
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                           "No connected displays.");
      display_update_sub (qs, NULL, NULL);
    }

  g_free (sel);
  qs->updating = FALSE;
  qs_unref (qs);
}

static void
display_load_async (QuickSettings *qs)
{
  if (!command_exists ("xrandr"))
    {
      display_unavailable (qs);
      return;
    }
  if (qs->display_loading)
    return;
  qs->display_loading = TRUE;
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status), "Loading…");
  run_command_async ("xrandr --query", display_query_done, qs);
}

static void
on_mon_changed (GtkComboBox *combo,
                gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *id;
  GList         *modes;
  gchar         *cur = NULL;
  const gchar   *shown;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  id = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor (id) || qs->display_last == NULL)
    return;
  qs->updating = TRUE;
  modes = parse_modes (qs->display_last, id, &cur);
  shown = (cur != NULL) ? cur
          : (modes != NULL ? (const gchar *) modes->data : NULL);
  fill_combo (qs->mode_combo, modes, shown, "No modes");
  /* Scale has no per-monitor cache in --query: reset to default like
   * display_query_done, so switching monitors never shows a stale
   * factor from the previous selection. Under updating, no apply fires. */
  {
    gsize i;

    gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (qs->scale_combo));
    for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
      {
        gchar *label = g_strdup_printf ("%sx", SCALE_FACTORS[i]);

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->scale_combo),
                                   SCALE_FACTORS[i], label);
        g_free (label);
      }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->scale_combo), "1");
    gtk_widget_set_sensitive (qs->scale_combo, TRUE);
  }
  /* Fresh selection clears transient status; the power-switch sync below
   * re-applies the single-display guard message when it still applies. */
  gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
  if (monitor_is_active (qs->display_last, id))
    display_update_sub (qs, id, shown);
  else
    display_update_sub (qs, id, "Off");
  display_update_power_switch (qs, id, qs->display_last);
  g_list_free_full (modes, g_free);
  g_free (cur);
  qs->updating = FALSE;
}

static void
on_mode_changed (GtkComboBox *combo,
                 gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *mon;
  const gchar   *mode;
  gchar         *qmon;
  gchar         *qmode;
  gchar         *cmd;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  mode = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor (mon) || !valid_mode (mode))
    return;
  qmon = g_shell_quote (mon);
  qmode = g_shell_quote (mode);
  cmd = g_strdup_printf ("xrandr --output %s --mode %s", qmon, qmode);
  g_free (qmon);
  g_free (qmode);
  spawn_command (cmd);
  g_free (cmd);
  display_update_sub (qs, mon, mode);
}

static void
on_scale_changed (GtkComboBox *combo,
                  gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *mon;
  const gchar   *factor;
  gchar         *qmon;
  gchar         *cmd;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  factor = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor (mon) || !valid_scale (factor))
    return;
  qmon = g_shell_quote (mon);
  cmd = g_strdup_printf ("xrandr --output %s --scale %sx%s",
                         qmon, factor, factor);
  g_free (qmon);
  spawn_command (cmd);
  g_free (cmd);
}

/* ---------------- monitor on/off (async apply + refresh) ----------------
 * ON  = xrandr --output MON --auto (preferred mode, always valid)
 * OFF = xrandr --output MON --off
 * The apply runs via run_command_async (worker thread, done() back on the
 * main loop): the panel never blocks. On success the cached --query is
 * refreshed, which resyncs combos, switch, tile sub and status. On
 * failure the switch is reverted in place and the error is shown. */

typedef struct
{
  QuickSettings *qs;
  gchar         *mon;
  gboolean       turn_on;
} MonPower;

static void
mon_power_done (const gchar *output,
                const gchar *errout,
                gboolean     ok,
                gpointer     user_data)
{
  MonPower *c = user_data;
  QuickSettings *qs = c->qs;

  (void) output;
  if (qs->dead)
    {
      g_free (c->mon);
      qs_unref (qs);
      g_free (c);
      return;
    }
  if (ok)
    {
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                          c->turn_on ? "Turning monitor on…"
                                     : "Turning monitor off…");
      display_load_async (qs);
    }
  else
    {
      gchar *detail = NULL;

      if (errout != NULL && *errout != '\0')
        {
          detail = g_strdup (errout);
          detail[strcspn (detail, "\n")] = '\0';
          g_strstrip (detail);
          if (*detail == '\0')
            {
              g_free (detail);
              detail = NULL;
            }
        }
      qs->updating = TRUE;
      if (qs->mon_switch != NULL)
        gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), !c->turn_on);
      qs->updating = FALSE;
      if (detail != NULL)
        {
          gchar *msg = g_strdup_printf ("Failed to turn monitor %s: %s",
                                        c->turn_on ? "on" : "off", detail);

          gtk_label_set_text (GTK_LABEL (qs->display_status), msg);
          g_free (msg);
          g_free (detail);
        }
      else
        {
          gtk_label_set_text (GTK_LABEL (qs->display_status),
                              c->turn_on ? "Failed to turn monitor on."
                                         : "Failed to turn monitor off.");
        }
    }
  g_free (c->mon);
  qs_unref (qs);
  g_free (c);
}

static void
mon_set_power_async (QuickSettings *qs,
                     const gchar   *mon,
                     gboolean       turn_on)
{
  MonPower *c;
  gchar    *qmon;
  gchar    *cmd;

  qmon = g_shell_quote (mon);
  if (turn_on)
    cmd = g_strdup_printf ("xrandr --output %s --auto", qmon);
  else
    cmd = g_strdup_printf ("xrandr --output %s --off", qmon);
  g_free (qmon);
  c = g_new0 (MonPower, 1);
  c->qs = qs;
  c->mon = g_strdup (mon);
  c->turn_on = turn_on;
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                      turn_on ? "Turning monitor on…"
                              : "Turning monitor off…");
  run_command_async (cmd, mon_power_done, c);
  g_free (cmd);
}

/* Same GtkSwitch pattern as Wi-Fi/Bluetooth (notify::active + updating
 * guard). Re-checks the single-display guard: a stale query (second
 * monitor unplugged meanwhile) must not black out the session. */
static void
on_mon_power_changed (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  QuickSettings *qs = user_data;
  gboolean       on = gtk_switch_get_active (GTK_SWITCH (obj));
  const gchar   *mon;
  gint           n_active = 0;

  (void) pspec;
  if (qs->updating)
    return;
  if (qs->mon_switch == NULL
      || !gtk_widget_is_sensitive (qs->mon_switch))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  if (!valid_monitor (mon))
    return;
  if (!on && qs->display_last != NULL)
    {
      count_displays (qs->display_last, NULL, &n_active);
      if (monitor_is_active (qs->display_last, mon) && n_active <= 1)
        {
          qs->updating = TRUE;
          gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), TRUE);
          gtk_widget_set_sensitive (qs->mon_switch, FALSE);
          gtk_widget_set_tooltip_text (qs->mon_switch,
                                       "Cannot disable the only display");
          qs->updating = FALSE;
          gtk_label_set_text (GTK_LABEL (qs->display_status),
                              "Cannot disable the only display.");
          return;
        }
    }
  mon_set_power_async (qs, mon, on);
}

/* ---------------- section: build + refresh ---------------- */

static void
on_night_tile_clicked (GtkButton *button,
                       gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "night");
}

static void
on_display_tile_clicked (GtkButton *button,
                         gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  display_load_async (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "display");
}

static void
on_display_back_clicked (GtkButton *button,
                         gpointer   user_data)
{
  (void) button;
  show_main_page (user_data);
}

static void
on_night_back_clicked (GtkButton *button,
                       gpointer   user_data)
{
  (void) button;
  night_update_sub (user_data);
  show_main_page (user_data);
}

void
display_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *display_grid = make_group (box, "Display");
  GtkWidget *tile_box;
  GtkWidget *night_page;
  GtkWidget *preset_box;
  GtkWidget *night_back;
  GtkWidget *disp_page;
  GtkWidget *mon_grid;
  GtkWidget *mode_grid;
  GtkWidget *scale_grid;
  GtkWidget *disp_back;

  make_slider_row (display_grid, 0, 100, "display-brightness-symbolic",
                   "Screen brightness",
                   NULL, &qs->bright_scale);
  /* Piso visual igual ao do setter: impede arrastar até 0% (preto). */
  gtk_range_set_range (GTK_RANGE (qs->bright_scale),
                       BRIGHTNESS_MIN, 100.0);

  /* Two half-width tiles sharing one row, like Wi-Fi + Bluetooth. */
  tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (display_grid), tile_box, 0, 1, 2, 1);

  /* Tile on the main page; presets live on the sub-page. */
  qs->night_tile = make_nav_tile (tile_box, "weather-clear-night-symbolic",
                                  "Night Light", &qs->night_sub);
  gtk_label_set_text (GTK_LABEL (qs->night_sub), "Off");
  g_signal_connect (qs->night_tile, "clicked",
                    G_CALLBACK (on_night_tile_clicked), qs);

  qs->display_tile = make_nav_tile (tile_box, "video-display-symbolic",
                                    "Display", &qs->display_sub);
  gtk_label_set_text (GTK_LABEL (qs->display_sub), "…");
  g_signal_connect (qs->display_tile, "clicked",
                    G_CALLBACK (on_display_tile_clicked), qs);

  /* Sub-page with the color temperature presets. */
  night_page = make_sub_page ("Night Light", &night_back);
  /* Presets stay compact (qs-compact + tight inner padding); the box gap
   * is the midpoint tight(4)+inner(8) = 6: breathing room without going
   * back to the old 8px rhythm. */
  preset_box = gtk_box_new (GTK_ORIENTATION_VERTICAL,
                            (QS_TIGHT_GAP + QS_INNER_GAP) / 2);
  gtk_box_pack_start (GTK_BOX (night_page), preset_box, FALSE, FALSE, 0);
  qs->night_day = make_night_button (preset_box,
                                     "weather-clear-symbolic",
                                     "Day", "6500K", qs);
  qs->night_aft = make_night_button (preset_box,
                                     "weather-few-clouds-symbolic",
                                     "Afternoon", "5500K", qs);
  qs->night_night = make_night_button (preset_box,
                                       "weather-clear-night-symbolic",
                                       "Night", "4500K", qs);
  qs->night_dawn = make_night_button (preset_box,
                                      "weather-few-clouds-night-symbolic",
                                      "Dawn", "3500K", qs);
  gtk_widget_set_sensitive (preset_box, night_available ());
  gtk_stack_add_named (GTK_STACK (stack), night_page, "night");
  g_signal_connect (night_back, "clicked",
                    G_CALLBACK (on_night_back_clicked), qs);

  /* Sub-page with the monitor / resolution / scale selectors. */
  disp_page = make_sub_page ("Display", &disp_back);
  mon_grid = make_group (disp_page, "Monitor");
  qs->mon_combo = make_combo_row (mon_grid, 0, "video-display-symbolic",
                                  "Monitor");
  /* Monitor on/off row: same GtkSwitch pattern as Wi-Fi/Bluetooth. */
  {
    GtkWidget *power_icon;
    GtkWidget *power_box;
    GtkWidget *power_label;

    power_icon = gtk_image_new_from_icon_name ("video-display-symbolic",
                                               GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size (GTK_IMAGE (power_icon), QS_ROW_ICON_PX);
    gtk_widget_set_halign (power_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (power_icon, GTK_ALIGN_CENTER);
    gtk_grid_attach (GTK_GRID (mon_grid), power_icon, 0, 1, 1, 1);

    power_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
    gtk_widget_set_hexpand (power_box, TRUE);
    power_label = gtk_label_new ("Monitor on/off");
    gtk_widget_set_halign (power_label, GTK_ALIGN_START);
    gtk_widget_set_valign (power_label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand (power_label, TRUE);
    gtk_box_pack_start (GTK_BOX (power_box), power_label, TRUE, TRUE, 0);
    qs->mon_switch = gtk_switch_new ();
    gtk_widget_set_halign (qs->mon_switch, GTK_ALIGN_END);
    gtk_widget_set_valign (qs->mon_switch, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
    /* First query enables it (avoids a brief wrong OFF affordance). */
    gtk_widget_set_sensitive (qs->mon_switch, FALSE);
    gtk_box_pack_start (GTK_BOX (power_box), qs->mon_switch,
                        FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (mon_grid), power_box, 1, 1, 1, 1);
  }
  ui_add_separator (disp_page);
  mode_grid = make_group (disp_page, "Resolution");
  qs->mode_combo = make_combo_row (mode_grid, 0,
                                   "preferences-desktop-display-symbolic",
                                   "Resolution");
  ui_add_separator (disp_page);
  scale_grid = make_group (disp_page, "Scale");
  qs->scale_combo = make_combo_row (scale_grid, 0,
                                    "view-fullscreen-symbolic",
                                    "Display scale");
  qs->display_status = gtk_label_new (NULL);
  ui_add_class (qs->display_status, "qs-status");
  gtk_label_set_line_wrap (GTK_LABEL (qs->display_status), TRUE);
  gtk_widget_set_halign (qs->display_status, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (disp_page), qs->display_status,
                      FALSE, FALSE, 0);
  gtk_stack_add_named (GTK_STACK (stack), disp_page, "display");
  g_signal_connect (disp_back, "clicked",
                    G_CALLBACK (on_display_back_clicked), qs);

  g_signal_connect (qs->mon_combo, "changed",
                    G_CALLBACK (on_mon_changed), qs);
  g_signal_connect (qs->mon_switch, "notify::active",
                    G_CALLBACK (on_mon_power_changed), qs);
  g_signal_connect (qs->mode_combo, "changed",
                    G_CALLBACK (on_mode_changed), qs);
  g_signal_connect (qs->scale_combo, "changed",
                    G_CALLBACK (on_scale_changed), qs);

  g_signal_connect (qs->bright_scale, "value-changed",
                    G_CALLBACK (on_brightness_changed), qs);
}

void
display_refresh_brightness (QuickSettings *qs)
{
  gint brightness;

  qs->updating = TRUE;

  brightness = get_brightness_percent ();
  if (brightness >= 0)
    {
      /* Se o sistema está abaixo do piso (ex: zerado por fora),
       * mostra o piso: mover o slider já recupera para >= MIN. */
      gtk_range_set_value (GTK_RANGE (qs->bright_scale),
                           CLAMP (brightness, BRIGHTNESS_MIN, 100));
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

/* Tile sensitivity + subtitle for external monitors. Without xrandr the
 * page stays "unavailable"; otherwise (re)probes asynchronously. */
void
display_refresh_tile (QuickSettings *qs)
{
  night_update_sub (qs);
  if (!command_exists ("xrandr"))
    {
      display_unavailable (qs);
      return;
    }
  gtk_widget_set_sensitive (qs->display_tile, TRUE);
  display_load_async (qs);
}
