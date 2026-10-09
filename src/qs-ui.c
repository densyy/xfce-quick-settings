#include "qs-ui.h"

/* ---------------- UI ---------------- */

/* Creates a titled group (header label + content grid) inside a vertical
 * box. Returns the content grid for attaching rows. */
GtkWidget *
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

void
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

  *scale_out = scale;
  if (icon_out != NULL)
    *icon_out = icon;
}

GtkWidget *
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
void
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
/* ---------------- connection pages ---------------- */

void
set_tile_active (GtkWidget *tile,
                 gboolean   on)
{
  GtkStyleContext *ctx = gtk_widget_get_style_context (tile);

  if (on)
    gtk_style_context_add_class (ctx, "qs-tile-active");
  else
    gtk_style_context_remove_class (ctx, "qs-tile-active");
}

GtkWidget *
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
GtkWidget *
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

void
clear_list_box (GtkWidget *list)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (list));
  GList *l;

  for (l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);
}

GtkWidget *
make_placeholder_row (const gchar *text)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_container_add (GTK_CONTAINER (row), label);
  gtk_widget_set_sensitive (row, FALSE);
  return row;
}

/* Accent background for active tiles (Wi-Fi / Bluetooth). */
void
ui_load_css (void)
{
  static gboolean  css_loaded = FALSE;
  GtkCssProvider  *provider;

  if (css_loaded)
    return;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider,
    ".qs-tile-active { background-color: @theme_selected_bg_color;"
    " color: @theme_selected_fg_color; }", -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
    GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
  css_loaded = TRUE;
}

/* Sub-page shell: header (back + bold title), custom content packed below. */
GtkWidget *
make_sub_page (const gchar *title,
               GtkWidget  **back_out)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *back = gtk_button_new_from_icon_name ("go-previous-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  GtkWidget *title_label = gtk_label_new (NULL);
  gchar     *markup = g_strdup_printf ("<b>%s</b>", title);

  gtk_container_set_border_width (GTK_CONTAINER (page), 12);
  gtk_label_set_markup (GTK_LABEL (title_label), markup);
  g_free (markup);
  gtk_widget_set_hexpand (title_label, TRUE);
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (header), back, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (header), title_label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (page), header, FALSE, FALSE, 0);

  *back_out = back;
  return page;
}

void
show_main_page (QuickSettings *qs)
{
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
}
