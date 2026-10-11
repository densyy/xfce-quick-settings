/* qs-wifi-backend.h — Wi-Fi nmcli backend (movido de qs-wifi.c). */

#ifndef QS_WIFI_BACKEND_H
#define QS_WIFI_BACKEND_H

#include "qs-core.h"

typedef struct
{
  gchar    *ssid;
  gint      signal;
  gboolean  secured;
  gboolean  active;
} WifiNet;

void         wifi_net_free     (gpointer data);
gboolean     wifi_available    (void);
gboolean     wifi_enabled_sync (void);
void         wifi_set_enabled  (gboolean on);
GList       *parse_wifi_list   (const gchar *output);
const gchar *wifi_signal_icon  (gint signal);

#endif /* QS_WIFI_BACKEND_H */
