#include "qs-ui.h"

/* ---------------- UI ---------------- */

void
ui_add_class (GtkWidget   *widget,
              const gchar *cls)
{
  gtk_style_context_add_class (gtk_widget_get_style_context (widget), cls);
}

/* Creates a titled group (header label + content grid) inside a vertical
 * box. Returns the content grid for attaching rows. */
GtkWidget *
make_group (GtkWidget   *box,
            const gchar *title)
{
  GtkWidget *header;
  GtkWidget *grid;

  header = gtk_label_new (title);
  ui_add_class (header, "qs-section");
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (box), header, FALSE, FALSE, 0);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), QS_INNER_GAP);
  gtk_grid_set_column_spacing (GTK_GRID (grid), QS_PAGE_PAD);
  gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

  return grid;
}

GtkWidget *
make_plain_grid (GtkWidget *box)
{
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), QS_INNER_GAP);
  gtk_grid_set_column_spacing (GTK_GRID (grid), QS_PAGE_PAD);
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
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_MAIN_ICON_PX);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), icon, 0, row, 1, 1);

  scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, max, 1);
  ui_add_class (scale, "qs-slider");
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
  ui_add_class (button, "qs-tile");
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 4);
  image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_image_set_pixel_size (GTK_IMAGE (image), QS_MAIN_ICON_PX);
  text = gtk_label_new (label);
  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), text, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_size_request (button, -1, 48);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

GtkWidget *
make_nav_tile (GtkWidget    *box,
               const gchar  *icon_name,
               const gchar  *title,
               GtkWidget   **sub_out)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                   GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title_label = gtk_label_new (title);
  GtkWidget *sub = NULL;

  ui_add_class (button, "qs-tile");
  gtk_image_set_pixel_size (GTK_IMAGE (image), QS_MAIN_ICON_PX);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), QS_CONTENT_PAD);
  ui_add_class (title_label, "qs-title");
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (vbox), title_label, FALSE, FALSE, 0);
  if (sub_out != NULL)
    {
      sub = gtk_label_new ("");
      ui_add_class (sub, "qs-sub");
      gtk_widget_set_halign (sub, GTK_ALIGN_START);
      gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
      gtk_box_pack_start (GTK_BOX (vbox), sub, FALSE, FALSE, 0);
      *sub_out = sub;
    }
  gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), vbox, TRUE, TRUE, 0);
  gtk_container_add (GTK_CONTAINER (button), hbox);
  gtk_widget_set_size_request (button, -1, 56);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

GtkWidget *
make_mini_tile (GtkWidget    *box,
                const gchar  *icon_name,
                const gchar  *title,
                GtkWidget   **sub_out)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                   GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *title_label = gtk_label_new (title);
  GtkWidget *sub = NULL;

  ui_add_class (button, "qs-tile");
  gtk_image_set_pixel_size (GTK_IMAGE (image), QS_MAIN_ICON_PX);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), QS_CONTENT_PAD);
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  ui_add_class (title_label, "qs-mini-title");
  gtk_widget_set_halign (title_label, GTK_ALIGN_CENTER);
  gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (vbox), image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), title_label, FALSE, FALSE, 0);
  if (sub_out != NULL)
    {
      sub = gtk_label_new ("");
      ui_add_class (sub, "qs-sub");
      gtk_widget_set_halign (sub, GTK_ALIGN_CENTER);
      gtk_box_pack_start (GTK_BOX (vbox), sub, FALSE, FALSE, 0);
      *sub_out = sub;
    }
  gtk_container_add (GTK_CONTAINER (button), vbox);
  gtk_widget_set_size_request (button, -1, 84);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

/* Meter card: dim name + bold value header, thin bar below. */
void
make_meter (GtkWidget    *grid,
            gint          row,
            const gchar  *name,
            GtkWidget   **value_out,
            GtkWidget   **bar_out)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *name_label = gtk_label_new (name);
  GtkWidget *value = gtk_label_new ("—");
  GtkWidget *bar = gtk_progress_bar_new ();

  ui_add_class (card, "qs-card");
  gtk_container_set_border_width (GTK_CONTAINER (card), QS_CONTENT_PAD);
  ui_add_class (name_label, "qs-sub");
  gtk_widget_set_halign (name_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand (name_label, TRUE);
  ui_add_class (value, "qs-title");
  gtk_widget_set_halign (value, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (header), name_label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (header), value, FALSE, FALSE, 0);
  ui_add_class (bar, "qs-meter");
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (bar), FALSE);
  gtk_widget_set_hexpand (bar, TRUE);
  gtk_box_pack_start (GTK_BOX (card), header, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (card), bar, FALSE, FALSE, 0);
  gtk_grid_attach (GTK_GRID (grid), card, 0, row, 2, 1);

  if (value_out != NULL)
    *value_out = value;
  if (bar_out != NULL)
    *bar_out = bar;
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
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_PAGE_PAD);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *back = gtk_button_new_from_icon_name ("go-previous-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  GtkWidget *title_label = gtk_label_new (title);
  GtkWidget *sw = gtk_switch_new ();
  GtkWidget *scan = gtk_button_new_with_label (scan_label);
  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *list = gtk_list_box_new ();
  GtkWidget *status = gtk_label_new (NULL);

  ui_add_class (back, "qs-back");
  gtk_container_set_border_width (GTK_CONTAINER (page), QS_PAGE_PAD);
  ui_add_class (title_label, "qs-page-title");
  gtk_widget_set_hexpand (title_label, TRUE);
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (header), back, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (header), title_label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (header), sw, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (page), header, FALSE, FALSE, 0);
  ui_add_class (scan, "qs-btn");
  gtk_box_pack_start (GTK_BOX (page), scan, FALSE, FALSE, 0);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scroll, -1, 220);
  ui_add_class (list, "qs-list");
  gtk_container_add (GTK_CONTAINER (scroll), list);
  gtk_box_pack_start (GTK_BOX (page), scroll, TRUE, TRUE, 0);
  ui_add_class (status, "qs-status");
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

  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  ui_add_class (label, "qs-status");
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_top (label, QS_INNER_GAP);
  gtk_widget_set_margin_bottom (label, QS_INNER_GAP);
  gtk_widget_set_margin_start (label, QS_INNER_GAP);
  gtk_widget_set_margin_end (label, QS_INNER_GAP);
  gtk_container_add (GTK_CONTAINER (row), label);
  gtk_widget_set_sensitive (row, FALSE);
  return row;
}

/* Shared visual language: one radius, theme-aware colors, type scale.
 * Every screen uses only these classes, so all pages share one token. */
void
ui_load_css (void)
{
  static gboolean  css_loaded = FALSE;
  GtkCssProvider  *provider;

  if (css_loaded)
    return;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider,
    ".qs-section { font-weight: bold; font-size: smaller;"
    " color: alpha(@theme_fg_color, 0.6); }"
    ".qs-page-title { font-weight: bold; font-size: larger; }"
    ".qs-title { font-weight: bold; }"
    ".qs-mini-title { font-weight: bold; font-size: smaller; }"
    ".qs-sub, .qs-status { color: alpha(@theme_fg_color, 0.6);"
    " font-size: smaller; }"
    ".qs-value { font-weight: bold; font-size: larger; }"
    ".qs-back { min-width: 32px; min-height: 32px; padding: 0;"
    " border-radius: 999px; }"
    ".qs-tile, .qs-btn { border-radius: 10px; }"
    ".qs-tile { padding: 6px 8px; }"
    ".qs-btn { min-height: 40px; }"
    ".qs-tile-active, button.qs-tile:checked {"
    " background-color: alpha(@theme_selected_bg_color, 0.22);"
    " border-color: alpha(@theme_selected_bg_color, 0.6);"
    " color: @theme_fg_color; }"
    ".qs-card { background-color: @theme_base_color;"
    " border: 1px solid @borders; border-radius: 10px; }"
    ".qs-notice { background-color: @theme_bg_color;"
    " border: 1px solid @borders; border-radius: 10px; }"
    "list.qs-list { background-color: transparent; border: none; }"
    "list.qs-list row { background-image: none; background-color: transparent;"
    " border: none; box-shadow: none; padding: 0; }"
    "list.qs-list row:selected { background-color: transparent; }"
    "scale.qs-slider trough { min-height: 6px; border-radius: 3px; }"
    "scale.qs-slider highlight { min-height: 6px; border-radius: 3px;"
    " background-color: @theme_selected_bg_color; border: none; }"
    "scale.qs-slider slider { min-width: 16px; min-height: 16px;"
    " border-radius: 999px; }"
    ".qs-meter trough { min-height: 8px; border-radius: 4px; }"
    ".qs-meter trough progress { min-height: 8px; border-radius: 4px;"
    " background-color: @theme_selected_bg_color; border: none; }"
    ".qs-combo { min-height: 40px; }", -1, NULL);
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
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_PAGE_PAD);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *back = gtk_button_new_from_icon_name ("go-previous-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  GtkWidget *title_label = gtk_label_new (title);

  ui_add_class (back, "qs-back");
  gtk_container_set_border_width (GTK_CONTAINER (page), QS_PAGE_PAD);
  ui_add_class (title_label, "qs-page-title");
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
