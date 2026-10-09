#include "qs-display.h"
#include "qs-ui.h"
#include "qs-util.h"

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

/* ---------------- section: build + refresh ---------------- */

void
display_build_section (GtkWidget *box, QuickSettings *qs)
{
  GtkWidget *display_grid = make_group (box, "Display");
  GtkWidget *night_box;

  make_slider_row (display_grid, 0, 100, "display-brightness-symbolic",
                   "Screen brightness",
                   NULL, &qs->bright_scale);

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
