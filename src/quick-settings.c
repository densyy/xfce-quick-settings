#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct
{
  XfcePanelPlugin *plugin;
  GtkWidget       *button;
  GtkWidget       *button_image;
  GtkWidget       *popup;
} QuickSettings;

static void
position_popup (QuickSettings *qs)
{
  GdkWindow    *button_window;
  GdkDisplay   *display;
  GdkMonitor   *monitor_obj;
  GdkRectangle  monitor;
  GtkAllocation allocation;
  gint          origin_x = 0;
  gint          origin_y = 0;
  gint          popup_width;
  gint          popup_height;
  gint          x;
  gint          y;

  button_window = gtk_widget_get_window (qs->button);
  if (button_window == NULL)
    return;

  gtk_widget_get_allocation (qs->button, &allocation);
  gdk_window_get_origin (button_window, &origin_x, &origin_y);

  display = gtk_widget_get_display (qs->button);
  monitor_obj = gdk_display_get_monitor_at_window (display, button_window);
  if (monitor_obj == NULL)
    monitor_obj = gdk_display_get_monitor (display, 0);
  if (monitor_obj != NULL)
    gdk_monitor_get_workarea (monitor_obj, &monitor);
  else
    monitor = (GdkRectangle) { 0, 0, 1024, 768 };

  gtk_window_get_size (GTK_WINDOW (qs->popup), &popup_width, &popup_height);
  if (popup_width <= 1 || popup_height <= 1)
    {
      popup_width = 300;
      popup_height = 200;
    }

  x = origin_x + allocation.x + allocation.width - popup_width;
  y = origin_y + allocation.y + allocation.height + 6;

  if (x < monitor.x + 6)
    x = monitor.x + 6;
  if (x + popup_width > monitor.x + monitor.width - 6)
    x = monitor.x + monitor.width - popup_width - 6;

  if (y + popup_height > monitor.y + monitor.height - 6)
    y = origin_y + allocation.y - popup_height - 6;
  if (y < monitor.y + 6)
    y = monitor.y + 6;

  gtk_window_move (GTK_WINDOW (qs->popup), x, y);
}

static void
on_button_clicked (GtkButton *button,
                   gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;

  if (gtk_widget_get_visible (qs->popup))
    {
      gtk_widget_hide (qs->popup);
      return;
    }

  gtk_widget_show_all (qs->popup);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  position_popup (qs);
  gtk_window_present (GTK_WINDOW (qs->popup));
}

static gboolean
on_size_changed (XfcePanelPlugin *plugin,
                 gint             size,
                 gpointer         user_data)
{
  QuickSettings *qs = user_data;
  gint           icon_size = xfce_panel_plugin_get_icon_size (plugin);

  gtk_image_set_pixel_size (GTK_IMAGE (qs->button_image), MAX (16, icon_size));
  gtk_widget_set_size_request (qs->button, size, size);
  return TRUE;
}

static void
quick_settings_free (XfcePanelPlugin *plugin,
                     gpointer         user_data)
{
  QuickSettings *qs = user_data;

  (void) plugin;

  g_free (qs);
}

static void
construct_quick_settings (XfcePanelPlugin *plugin)
{
  QuickSettings *qs;
  GtkWidget     *label;

  qs = g_new0 (QuickSettings, 1);
  qs->plugin = plugin;

  xfce_panel_plugin_set_small (plugin, TRUE);

  qs->button = gtk_button_new ();
  qs->button_image = gtk_image_new_from_icon_name ("preferences-system-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
  gtk_button_set_relief (GTK_BUTTON (qs->button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text (qs->button, "Quick Settings");
  gtk_container_add (GTK_CONTAINER (qs->button), qs->button_image);
  gtk_container_add (GTK_CONTAINER (plugin), qs->button);

  qs->popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated (GTK_WINDOW (qs->popup), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (qs->popup), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_keep_above (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_type_hint (GTK_WINDOW (qs->popup), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_widget_set_size_request (qs->popup, 300, -1);

  label = gtk_label_new ("Hello from Quick Settings");
  gtk_container_add (GTK_CONTAINER (qs->popup), label);

  g_signal_connect (qs->button, "clicked", G_CALLBACK (on_button_clicked), qs);
  g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), qs);
  g_signal_connect (plugin, "free-data", G_CALLBACK (quick_settings_free), qs);

  on_size_changed (plugin, xfce_panel_plugin_get_size (plugin), qs);

  gtk_widget_show_all (GTK_WIDGET (plugin));
}

XFCE_PANEL_PLUGIN_REGISTER (construct_quick_settings)
