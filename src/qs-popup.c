#include "qs-popup.h"

void
qs_popup_position (QuickSettings *qs)
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
      popup_width = 320;
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

gboolean
qs_popup_pointer_on_button (QuickSettings *qs)
{
  GdkDisplay    *display;
  GdkSeat       *seat;
  GdkDevice     *pointer;
  GdkWindow     *button_window;
  GtkAllocation  allocation;
  gint           x = -1;
  gint           y = -1;

  if (qs->button == NULL)
    return FALSE;
  button_window = gtk_widget_get_window (qs->button);
  if (button_window == NULL)
    return FALSE;
  display = gtk_widget_get_display (qs->button);
  seat = gdk_display_get_default_seat (display);
  if (seat == NULL)
    return FALSE;
  pointer = gdk_seat_get_pointer (seat);
  if (pointer == NULL)
    return FALSE;
  gdk_window_get_device_position (button_window, pointer, &x, &y, NULL);
  gtk_widget_get_allocation (qs->button, &allocation);
  return (x >= 0 && y >= 0 && x < allocation.width && y < allocation.height);
}

/* Clicking outside the popup moves focus to another toplevel: hide.
 * Clicks inside only move focus between children, so no event fires.
 * While an in-process grab is active (combo dropdown open, button being
 * pressed) the focus change belongs to that interaction: let it finish
 * instead of hiding underneath it. A focus-out caused by pressing the
 * panel button is also skipped so "clicked" can toggle normally. */
gboolean
qs_popup_on_focus_out (GtkWidget     *widget,
                       GdkEventFocus *event,
                       gpointer       user_data)
{
  QuickSettings *qs = user_data;

  (void) widget;
  (void) event;

  if (!gtk_widget_get_visible (qs->popup))
    return FALSE;
  if (gtk_grab_get_current () != NULL)
    return FALSE;
  if (qs_popup_pointer_on_button (qs))
    return FALSE;
  gtk_widget_hide (qs->popup);
  return FALSE;
}

gboolean
qs_popup_on_key_press (GtkWidget   *widget,
                       GdkEventKey *event,
                       gpointer     user_data)
{
  QuickSettings *qs = user_data;

  (void) widget;

  if (event->keyval == GDK_KEY_Escape
      && gtk_widget_get_visible (qs->popup))
    {
      gtk_widget_hide (qs->popup);
      return TRUE;
    }
  return FALSE;
}
