#ifndef QS_BT_BACKEND_H
#define QS_BT_BACKEND_H

#include "qs-core.h"

typedef struct
{
  gchar    *mac;
  gchar    *name;
  gboolean  paired;
  gboolean  trusted;
  gboolean  connected;
} BtDevice;

void bt_device_free (gpointer data);
gchar *bt_get_controller (void);
gboolean bt_powered (const gchar *controller);
void bt_set_powered (gboolean on);
void bt_fill_flags (BtDevice *dev);
gboolean bt_str_contains_ci (const gchar *haystack,
                             const gchar *needle);
gboolean bt_output_is_error (const gchar *output,
                             const gchar *errout);
GList *list_bt_devices (void);

#endif /* QS_BT_BACKEND_H */
