/* qs-nightlight.c — night-light responsibility (redshift > gammastep).
 *
 * TU própria (Makefile SRC); compartilha a fachada qs-display.h.
 */
#include "qs-display.h"
#include "qs-ui.h"
#include "qs-util.h"

gboolean
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

void
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

GtkWidget *
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
