#ifndef QS_NET_H
#define QS_NET_H

#include "qs-core.h"

/* Connection facade. Implementation split (SRP):
 * - src/qs-wifi.c: nmcli backend + Wi-Fi page (wifi_show_page,
 *   wifi_update_tile, wifi_build_page, wifi_free_cache).
 * - src/qs-bluetooth.c: bluetoothctl backend + BT page + tile probe
 *   (bt_show_page, bt_update_tile_cached, bt_refresh_tile_async,
 *   bt_build_page, bt_free_caches, bt_cancel_all).
 * This header stays the stable facade used by qs-plugin.c, plus the
 * module-public symbols used by the facade (qs-net.c). */

/* Builds the Connection group (Wi-Fi/Bluetooth tiles + sub-pages). */
void net_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);
/* Live tile state (sensitive/active) for Wi-Fi and Bluetooth. */
void net_update_tiles (QuickSettings *qs);
/* Async live probe of the Bluetooth tile (controller powered state).
 * Safe to call on every popup open; never blocks the main loop. */
void net_refresh_bt_tile (QuickSettings *qs);
/* Releases the Wi-Fi saved-profile cache + BT device cache/busy map and
 * flags in-flight BtLink chains cancelled (drops pending verify
 * timeouts). Idempotent; safe to call twice. Wired from plugin free-data
 * (quick_settings_free in qs-plugin.c) before destroying the popup.
 * Fallback: per-module destructors also free the caches at unload. */
void net_cleanup (void);

/* --- Wi-Fi module (qs-wifi.c), used by the facade --- */
void wifi_show_page (QuickSettings *qs);
void wifi_update_tile (QuickSettings *qs);
void wifi_build_page (GtkWidget     *stack,
                      QuickSettings *qs,
                      GtkWidget    **back_out);
void wifi_free_cache (void);

/* --- Bluetooth module (qs-bluetooth.c), used by the facade --- */
void bt_show_page (QuickSettings *qs);
void bt_update_tile_cached (QuickSettings *qs);
void bt_refresh_tile_async (QuickSettings *qs);
void bt_build_page (GtkWidget     *stack,
                    QuickSettings *qs,
                    GtkWidget    **back_out);
void bt_free_caches (void);
void bt_cancel_all (void);

#endif /* QS_NET_H */
