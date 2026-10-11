/* qs-display.c — fachada estável do módulo Display.
 *
 * SRP: as 3 responsabilidades vivem em TUs próprias:
 *   qs-brightness.c — backlight (pick/get/set, slider, refresh async)
 *   qs-nightlight.c — night-light (redshift/gammastep, presets)
 *   qs-monitor.c    — monitores xrandr (snapshot único, combos, power)
 * Este arquivo só compõe a seção (display_build_section).
 */
#include "qs-display.h"
#include "qs-ui.h"
#include "qs-util.h"

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
  gtk_range_set_range (GTK_RANGE (qs->bright_scale),
                       BRIGHTNESS_MIN, 100.0);

  tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (display_grid), tile_box, 0, 1, 2, 1);

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

  night_page = make_sub_page ("Night Light", &night_back);
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

  disp_page = make_sub_page ("Display", &disp_back);
  mon_grid = make_group (disp_page, "Monitor");
  qs->mon_combo = make_combo_row (mon_grid, 0, "video-display-symbolic",
                                  "Monitor");
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
