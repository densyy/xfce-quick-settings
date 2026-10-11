#include "qs-cache.h"
#include "qs-core.h"
#include "qs-ui.h"
#include "qs-popup.h"
#include "qs-audio.h"
#include "qs-display.h"
#include "qs-net.h"
#include "qs-power.h"
#include "qs-session.h"
#include "qs-notify.h"
#include "qs-sysmon.h"

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

  /* T3: render-from-cache primeiro (<1ms, via INI). Aplica tiles/subs
   * a partir do INI fresco; chaves stale/ausentes ficam com "…". */
  qs_cache_load (qs);
  /* Badge "atualizando…" só p/ stale/ausente: não sobrescreve valor fresco. */
  if (qs->wifi_sub != NULL)
    {
      const gchar *cur = gtk_label_get_text (GTK_LABEL (qs->wifi_sub));

      if (cur == NULL || *cur == '\0' || g_strcmp0 (cur, "…") == 0)
        gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "atualizando…");
    }
  qs->updating = TRUE;

  /* Show IMEDIATO: nada bloqueante antes deste ponto. */
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
  gtk_widget_show_all (qs->popup);
  /* Position after show/size-allocate: show_all already allocated the
   * widgets, and qs_popup_position falls back to 320x200 on the first
   * map. No busy-loop drain here: pumping gtk_main_iteration reenters
   * widget signals/dialogs and can hide the popup underneath a grab. */
  qs_popup_position (qs);
  gtk_window_present (GTK_WINDOW (qs->popup));

  /* SÓ DEPOIS do show: dispara refreshes (T4/T5 tornam wifi/notify
   * não-bloqueantes; BT já é async). Cada done_cb/idle re-renderiza seu
   * tile; um único schedule_save com debounce persiste na main. */
  audio_refresh_volume (qs);
  display_refresh_brightness (qs);
  display_refresh_tile (qs);
  audio_refresh_devices (qs);
  net_update_tiles (qs);
  net_refresh_bt_tile (qs); /* async: corrects the BT tile once probed */
  notify_update_tile (qs);
  notify_refresh_tile_async (qs); /* T5: cached acima + correção async */
  power_start (qs); /* after show_all: 30s timer only with a battery */
  qs_cache_schedule_save (qs);
  qs->updating = FALSE;
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
  net_cleanup ();
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
                    G_CALLBACK (qs_popup_on_focus_out), qs);
  g_signal_connect (qs->popup, "key-press-event",
                    G_CALLBACK (qs_popup_on_key_press), qs);
  gtk_widget_set_size_request (qs->popup, 320, -1);

  qs->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (qs->stack),
                                 GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_container_add (GTK_CONTAINER (qs->popup), qs->stack);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  gtk_stack_add_named (GTK_STACK (qs->stack), box, "main");

  audio_build_section (box, qs->stack, qs);
  display_build_section (box, qs->stack, qs);
  net_build_section (box, qs->stack, qs);

  power_build_section (box, qs);

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
  display_refresh_tile (qs);
  audio_refresh_devices (qs);
  net_update_tiles (qs);
  net_refresh_bt_tile (qs);
  power_refresh (qs);

  gtk_widget_show_all (GTK_WIDGET (plugin));
}

XFCE_PANEL_PLUGIN_REGISTER (construct_quick_settings)
