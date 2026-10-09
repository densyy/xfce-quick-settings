#include "qs-power.h"
#include "qs-ui.h"
#include "qs-util.h"

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
void
power_refresh (QuickSettings *qs)
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
  {
    gchar *value = g_strdup_printf ("%d%%", percent);

    gtk_label_set_text (GTK_LABEL (qs->bat_label), value);
    g_free (value);
  }
  if (state != NULL && g_ascii_strcasecmp (state, "fully-charged") == 0)
    label = g_strdup ("Fully charged");
  else if (state != NULL && g_ascii_strcasecmp (state, "empty") == 0)
    label = g_strdup ("Empty");
  else if (charging)
    label = g_strdup ("Charging");
  else
    label = g_strdup ("Discharging");
  if (time != NULL && *time != '\0')
    {
      tip = g_strdup_printf ("%s · %s: %s", label,
                             charging ? "Time to full" : "Time to empty",
                             time);
      g_free (label);
      label = tip;
      tip = NULL;
    }
  gtk_label_set_text (GTK_LABEL (qs->bat_status), label);
  g_free (label);
  g_free (state);
  g_free (time);
}

/* ---------------- section: build ---------------- */

void
power_build_section (GtkWidget *box, QuickSettings *qs)
{
  GtkWidget *bat_header;
  GtkWidget *bat_card;
  GtkWidget *bat_hbox;
  GtkWidget *bat_vbox;

  bat_header = gtk_label_new ("Battery");
  ui_add_class (bat_header, "qs-section");
  gtk_widget_set_halign (bat_header, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (box), bat_header, FALSE, FALSE, 0);

  /* Highlight card: big percentage + status line. Hidden when
   * power_refresh finds no battery. */
  qs->bat_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (box), qs->bat_box, FALSE, FALSE, 0);
  bat_card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  ui_add_class (bat_card, "qs-card");
  gtk_box_pack_start (GTK_BOX (qs->bat_box), bat_card, FALSE, FALSE, 0);
  bat_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_PAGE_PAD);
  gtk_container_set_border_width (GTK_CONTAINER (bat_hbox), QS_CONTENT_PAD);
  qs->bat_icon = gtk_image_new_from_icon_name ("battery-missing-symbolic",
                                                GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_image_set_pixel_size (GTK_IMAGE (qs->bat_icon), 32);
  gtk_widget_set_halign (qs->bat_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (qs->bat_icon, GTK_ALIGN_CENTER);
  bat_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  qs->bat_label = gtk_label_new ("—");
  ui_add_class (qs->bat_label, "qs-value");
  gtk_widget_set_halign (qs->bat_label, GTK_ALIGN_START);
  qs->bat_status = gtk_label_new ("");
  ui_add_class (qs->bat_status, "qs-sub");
  gtk_widget_set_halign (qs->bat_status, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (bat_vbox), qs->bat_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (bat_vbox), qs->bat_status, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (bat_hbox), qs->bat_icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (bat_hbox), bat_vbox, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (bat_card), bat_hbox, FALSE, FALSE, 0);
}
