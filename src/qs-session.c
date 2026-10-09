#include "qs-session.h"
#include "qs-ui.h"
#include "qs-util.h"

/* xfce4-session-logout shows its own confirmation dialog by default. */
static void
session_run (const gchar *xfce_arg,
             const gchar *fallback)
{
  gchar *cmd;

  if (command_exists ("xfce4-session-logout"))
    cmd = g_strdup_printf ("xfce4-session-logout %s", xfce_arg);
  else
    cmd = g_strdup (fallback);
  spawn_command (cmd);
  g_free (cmd);
}

static void
on_power_action_clicked (GtkButton *button,
                         gpointer   user_data)
{
  const gchar *action = g_object_get_data (G_OBJECT (button), "qs-action");

  (void) user_data;

  if (g_strcmp0 (action, "halt") == 0)
    session_run ("--halt", "systemctl poweroff");
  else if (g_strcmp0 (action, "reboot") == 0)
    session_run ("--reboot", "systemctl reboot");
  else if (g_strcmp0 (action, "hibernate") == 0)
    session_run ("--hibernate", "systemctl hibernate");
  else if (g_strcmp0 (action, "suspend") == 0)
    session_run ("--suspend", "systemctl suspend");
  else if (g_strcmp0 (action, "logout") == 0)
    {
      if (command_exists ("xfce4-session-logout"))
        spawn_command ("xfce4-session-logout --logout");
      else
        {
          gchar *cmd = g_strdup_printf ("loginctl terminate-user %s",
                                        g_get_user_name ());
          spawn_command (cmd);
          g_free (cmd);
        }
    }
}

static void
on_power_tile_clicked (GtkButton *button,
                       gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "power");
}

static void
on_power_back_clicked (GtkButton *button,
                       gpointer   user_data)
{
  (void) button;
  show_main_page (user_data);
}

static void
make_power_button (GtkWidget   *box,
                   const gchar *icon_name,
                   const gchar *label,
                   const gchar *action)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                   GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *text = gtk_label_new (label);

  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), text, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_halign (hbox, GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (button), "qs-action", (gpointer) action);
  g_signal_connect (button, "clicked",
                    G_CALLBACK (on_power_action_clicked), NULL);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
}

void
session_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *power_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *power_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *power_tile;
  GtkWidget *power_page;
  GtkWidget *action_box;
  GtkWidget *power_back;

  /* Full-width tile at the bottom of the main page. */
  gtk_box_pack_start (GTK_BOX (box), power_box, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (power_box), power_sep, FALSE, FALSE, 2);
  power_tile = make_tile (power_box, "system-shutdown-symbolic", "Power");
  g_signal_connect (power_tile, "clicked",
                    G_CALLBACK (on_power_tile_clicked), qs);

  /* Sub-page with the session actions. */
  power_page = make_sub_page ("Power", &power_back);
  action_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_set_homogeneous (GTK_BOX (action_box), TRUE);
  gtk_box_pack_start (GTK_BOX (power_page), action_box, FALSE, FALSE, 0);
  make_power_button (action_box, "system-shutdown-symbolic",
                     "Shut Down", "halt");
  make_power_button (action_box, "system-reboot-symbolic",
                     "Restart", "reboot");
  make_power_button (action_box, "system-hibernate-symbolic",
                     "Hibernate", "hibernate");
  make_power_button (action_box, "system-suspend-symbolic",
                     "Suspend", "suspend");
  make_power_button (action_box, "system-log-out-symbolic",
                     "Log Out", "logout");
  gtk_stack_add_named (GTK_STACK (stack), power_page, "power");
  g_signal_connect (power_back, "clicked",
                    G_CALLBACK (on_power_back_clicked), qs);
}
