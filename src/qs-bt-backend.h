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

/* Tri-state result of bt_signal_event_matches. NO_MATCH covers both
 * path not belonging to the link and dictionary without Connected. */
typedef enum
{
  BT_SIGNAL_NO_MATCH = 0,
  BT_SIGNAL_CONNECTED_FALSE = 1,
  BT_SIGNAL_CONNECTED_TRUE = 2
} BtSignalMatch;

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

/* Pure match of a PropertiesChanged event to a link.
 * object_path is the path received in the message, mac is the link
 * MAC in AA:BB:CC:DD:EE:FF form, changed is the a{sv} dictionary
 * received in the message. All parameters are borrowed and never
 * stored; changed is never modified. Returns BT_SIGNAL_CONNECTED_TRUE
 * when the path suffix matches mac and Connected is true,
 * BT_SIGNAL_CONNECTED_FALSE when it matches and Connected is false,
 * BT_SIGNAL_NO_MATCH when the path does not belong to mac or the
 * dictionary has no boolean Connected entry. Performs no D-Bus call. */
BtSignalMatch bt_signal_event_matches (const gchar *object_path,
                                       const gchar *mac,
                                       GVariant    *changed);

#endif /* QS_BT_BACKEND_H */
