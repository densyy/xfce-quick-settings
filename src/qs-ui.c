#include "qs-ui.h"
#include "qs-theme.h"

void
ui_add_class (GtkWidget   *widget,
              const gchar *cls)
{
  gtk_style_context_add_class (gtk_widget_get_style_context (widget), cls);
}

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
  gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (scale, tooltip);
  gtk_grid_attach (GTK_GRID (grid), scale, 1, row, 1, 1);

  *scale_out = scale;
  if (icon_out != NULL)
    *icon_out = icon;
}

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
      gtk_label_set_width_chars (GTK_LABEL (sub), 12);
      gtk_label_set_max_width_chars (GTK_LABEL (sub), 14);
      gtk_widget_set_hexpand (sub, TRUE);
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
      gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
      gtk_label_set_width_chars (GTK_LABEL (sub), 12);
      gtk_label_set_max_width_chars (GTK_LABEL (sub), 14);
      gtk_widget_set_hexpand (sub, TRUE);
      gtk_box_pack_start (GTK_BOX (vbox), sub, FALSE, FALSE, 0);
      *sub_out = sub;
    }
  gtk_container_add (GTK_CONTAINER (button), vbox);
  gtk_widget_set_size_request (button, -1, 84);
  gtk_box_pack_start (GTK_BOX (box), button, TRUE, TRUE, 0);
  return button;
}

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

/* Reentrancy guard for clear_list_box: destroying rows can synchronously
 * emit list signals whose handlers refill the list (see bt_filling in
 * qs-net.c). A nested clear must not destroy rows out from under the
 * outer emission, so it unparents its snapshot now and destroys it idle. */
static gboolean ui_clearing = FALSE;

typedef struct
{
  GList *widgets; /* GtkWidget*, each g_object_ref'd */
} UiClearIdle;

static gboolean
ui_clear_idle_destroy (gpointer data)
{
  UiClearIdle *c = data;
  GList       *l;

  for (l = c->widgets; l != NULL; l = l->next)
    {
      gtk_widget_destroy (GTK_WIDGET (l->data));
      g_object_unref (l->data);
    }
  g_list_free (c->widgets);
  g_free (c);
  return G_SOURCE_REMOVE;
}

void
clear_list_box (GtkWidget *list)
{
  GList *children;
  GList *l;

  if (list == NULL || !GTK_IS_CONTAINER (list))
    return;

  children = gtk_container_get_children (GTK_CONTAINER (list));
  if (children == NULL)
    return;

  if (ui_clearing)
    {
      UiClearIdle *c = g_new0 (UiClearIdle, 1);

      /* Unparent now so the refill sees an empty list; the actual
       * destroy runs after the current signal emission unwinds. */
      for (l = children; l != NULL; l = l->next)
        {
          g_object_ref (l->data);
          gtk_container_remove (GTK_CONTAINER (list), GTK_WIDGET (l->data));
          c->widgets = g_list_prepend (c->widgets, l->data);
        }
      g_list_free (children);
      g_idle_add (ui_clear_idle_destroy, c);
      return;
    }

  ui_clearing = TRUE;
  for (l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);
  ui_clearing = FALSE;
}

void
ui_add_separator (GtkWidget *box)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  ui_add_class (sep, "qs-sep");
  gtk_widget_set_margin_top (sep, QS_INNER_GAP);
  gtk_widget_set_margin_bottom (sep, QS_INNER_GAP);
  gtk_box_pack_start (GTK_BOX (box), sep, FALSE, FALSE, 0);
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

/* Shim: the stylesheet lives in qs-theme.c (SRP). Kept so existing
 * callers (qs-plugin.c) don't churn. */
void
ui_load_css (void)
{
  qs_theme_load ();
}

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
