#include "qs-core.h"
#include "qs-ui.h"
#include "qs-audio.h"
#include "qs-display.h"
#include "qs-net.h"
#include "qs-power.h"
#include "qs-session.h"
#include "qs-notify.h"
#include "qs-sysmon.h"

void
qs_ref (QuickSettings *qs)
{
  g_atomic_int_inc (&qs->refcount);
}

void
qs_unref (QuickSettings *qs)
{
  if (g_atomic_int_dec_and_test (&qs->refcount))
    {
      g_free (qs->bt_controller);
      g_free (qs->sysmon_disk_key);
      g_free (qs);
    }
}

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

/* TRUE while the pointer is over the panel button (i.e. a focus-out was
 * caused by pressing the button itself, not by clicking elsewhere). */
static gboolean
popup_pointer_on_button (QuickSettings *qs)
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
static gboolean
on_popup_focus_out (GtkWidget     *widget,
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
  if (popup_pointer_on_button (qs))
    return FALSE;
  gtk_widget_hide (qs->popup);
  return FALSE;
}

static gboolean
on_popup_key_press (GtkWidget   *widget,
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

  audio_refresh_volume (qs);
  display_refresh_brightness (qs);
  audio_refresh_devices (qs);
  net_update_tiles (qs);
  net_refresh_bt_tile (qs); /* async: corrects the BT tile once probed */
  notify_update_tile (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
  gtk_widget_show_all (qs->popup);
  power_start (qs); /* after show_all: 30s timer only with a battery */
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

  qs->dead = TRUE;
  sysmon_stop (qs);
  power_stop (qs);
  gtk_widget_destroy (qs->popup);
  qs_unref (qs);
}

static void
construct_quick_settings (XfcePanelPlugin *plugin)
{
  QuickSettings *qs;
  GtkWidget     *box;

  qs = g_new0 (QuickSettings, 1);
  qs->refcount = 1;

  ui_load_css ();

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
  gtk_window_set_accept_focus (GTK_WINDOW (qs->popup), TRUE);
  gtk_window_set_focus_on_map (GTK_WINDOW (qs->popup), TRUE);
  gtk_widget_add_events (qs->popup, GDK_FOCUS_CHANGE_MASK | GDK_KEY_PRESS_MASK);
  g_signal_connect (qs->popup, "focus-out-event",
                    G_CALLBACK (on_popup_focus_out), qs);
  g_signal_connect (qs->popup, "key-press-event",
                    G_CALLBACK (on_popup_key_press), qs);
  gtk_widget_set_size_request (qs->popup, 320, -1);

  qs->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (qs->stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_container_add (GTK_CONTAINER (qs->popup), qs->stack);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  gtk_stack_add_named (GTK_STACK (qs->stack), box, "main");

  /* Main page rhythm comes from section headers + spacing only. */
  audio_build_section (box, qs->stack, qs);
  display_build_section (box, qs->stack, qs);
  net_build_section (box, qs->stack, qs);

  /* Battery card carries its own box (hidden when no battery exists). */
  power_build_section (box, qs);

  /* System / Notifications / Power share one homogeneous action grid. */
  {
    GtkWidget *action_grid = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_box_set_homogeneous (GTK_BOX (action_grid), TRUE);
    gtk_box_pack_start (GTK_BOX (box), action_grid, FALSE, FALSE, 0);
    sysmon_build_section (action_grid, qs->stack, qs);
    notify_build_section (action_grid, qs->stack, qs);
    session_build_section (action_grid, qs->stack, qs);
  }

  g_signal_connect (qs->button, "clicked", G_CALLBACK (on_button_clicked), qs);
  g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), qs);
  g_signal_connect (plugin, "free-data", G_CALLBACK (quick_settings_free), qs);

  on_size_changed (plugin, xfce_panel_plugin_get_size (plugin), qs);
  audio_refresh_volume (qs);
  display_refresh_brightness (qs);
  audio_refresh_devices (qs);
  net_update_tiles (qs);
  net_refresh_bt_tile (qs);
  power_refresh (qs);

  gtk_widget_show_all (GTK_WIDGET (plugin));
}

XFCE_PANEL_PLUGIN_REGISTER (construct_quick_settings)
