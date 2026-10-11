#include "qs-net.h"
#include "qs-ui.h"

/* SRP split: Wi-Fi lives in qs-wifi.c, Bluetooth in qs-bluetooth.c; this
 * file is only the Connection facade (tiles + delegation). */

static void
on_wifi_tile_clicked (GtkButton *button,
                      gpointer   user_data)
{
  (void) button;
  wifi_show_page ((QuickSettings *) user_data);
}

static void
on_bt_tile_clicked (GtkButton *button,
                    gpointer   user_data)
{
  (void) button;
  bt_show_page ((QuickSettings *) user_data);
}

static void
on_conn_back_clicked (GtkButton *button,
                      gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  net_update_tiles (qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "main");
}

void
net_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *conn_grid = make_group (box, "Connection");
  GtkWidget *tile_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,
                                     QS_INNER_GAP);
  GtkWidget *wifi_back;
  GtkWidget *bt_back;

  gtk_box_set_homogeneous (GTK_BOX (tile_box), TRUE);
  gtk_grid_attach (GTK_GRID (conn_grid), tile_box, 0, 0, 2, 1);
  qs->wifi_tile = make_nav_tile (tile_box, "network-wireless-symbolic",
                                 "Wi-Fi", &qs->wifi_sub);
  gtk_label_set_text (GTK_LABEL (qs->wifi_sub), "…");
  g_signal_connect (qs->wifi_tile, "clicked",
                    G_CALLBACK (on_wifi_tile_clicked), qs);
  qs->bt_tile = make_nav_tile (tile_box, "bluetooth-active-symbolic",
                                "Bluetooth", &qs->bt_sub);
  gtk_label_set_text (GTK_LABEL (qs->bt_sub), "…");
  g_signal_connect (qs->bt_tile, "clicked",
                    G_CALLBACK (on_bt_tile_clicked), qs);

  /* Sub-pages own their switch/scan/list signals (see qs-wifi.c /
   * qs-bluetooth.c); the facade only wires Back -> main. */
  wifi_build_page (stack, qs, &wifi_back);
  bt_build_page (stack, qs, &bt_back);
  g_signal_connect (wifi_back, "clicked",
                    G_CALLBACK (on_conn_back_clicked), qs);
  g_signal_connect (bt_back, "clicked",
                    G_CALLBACK (on_conn_back_clicked), qs);
}

void
net_update_tiles (QuickSettings *qs)
{
  /* T4: cached paint (sync-sem-spawn) now, async nmcli probe corrects
   * the tile moments after popup open — never blocks the open path. */
  wifi_update_tile (qs);
  wifi_refresh_tile_async (qs);
  /* Bluetooth never blocks here: cached state now, async probe (below)
   * corrects it moments after popup open. */
  bt_update_tile_cached (qs);
}

void
net_refresh_bt_tile (QuickSettings *qs)
{
  bt_refresh_tile_async (qs);
}

void
net_cleanup (void)
{
  bt_cancel_all ();
  wifi_free_cache ();
  bt_free_caches ();
}
