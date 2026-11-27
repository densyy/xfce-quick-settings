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

/* Standalone trust (RF-006) and rename (RF-007) helpers. All functions
 * are pure or synchronous via bluetoothctl and never touch GTK, so
 * tests can exercise them with the fakebin PATH prefix. MAC is
 * validated as AA:BB:CC:DD:EE:FF and shell-quoted before any spawn;
 * alias is stripped, length-checked and shell-quoted. */

/* Trust outcome with auth/agent classification for feedback. */
typedef enum
{
  BT_TRUST_OK = 0,
  BT_TRUST_FAILED = 1,
  BT_TRUST_AUTH_FAILED = 2,
  BT_TRUST_NO_AGENT = 3
} BtTrustStatus;

/* Rename outcome. NO_DEVICE covers unknown device text from
 * bluetoothctl; other values mirror the trust classification. */
typedef enum
{
  BT_RENAME_OK = 0,
  BT_RENAME_FAILED = 1,
  BT_RENAME_AUTH_FAILED = 2,
  BT_RENAME_NO_AGENT = 3,
  BT_RENAME_NO_DEVICE = 4
} BtRenameStatus;

/* Pure validators: MAC must be AA:BB:CC:DD:EE:FF (hex, 5 colons);
 * alias must strip to 1..128 chars without newline. */
gboolean bt_mac_is_valid (const gchar *mac);
gboolean bt_alias_is_valid (const gchar *alias);

/* Pure command builders (caller frees, NULL on invalid input).
 * Both use g_shell_quote, so spaces in alias stay one argv. */
gchar *bt_build_trust_command (const gchar *mac);
gchar *bt_build_rename_command (const gchar *mac,
                                const gchar *alias);

/* Pure output classifiers. AlreadyExists counts as success even when
 * wrapped in a Failed line (pair on an already-paired device). */
BtTrustStatus bt_classify_trust_output (const gchar *output,
                                        const gchar *errout);
BtRenameStatus bt_classify_rename_output (const gchar *output,
                                          const gchar *errout);

/* Synchronous trust via bluetoothctl. Returns BT_TRUST_OK on
 * success text, classified failure otherwise. */
BtTrustStatus bt_trust_device (const gchar *mac);

/* Named D-Bus fallback for rename on old BlueZ without set-alias.
 * Calls org.freedesktop.DBus.Properties.Set with
 * (org.bluez.Device1, Alias, alias) on
 * /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX via the system bus. Returns
 * TRUE on success, FALSE with error set otherwise. Named (not an
 * inline closure) so tests can invoke it directly. */
gboolean bt_rename_via_dbus (const gchar *mac,
                              const gchar *alias,
                              GError     **error);

/* Injectable D-Bus seam for rename, mirroring bt_signal_subscribe_fn.
 * bt_rename_device calls this when the set-alias verb fails; tests
 * replace it to prove the fallback without a system bus. Passing
 * NULL restores bt_rename_via_dbus. */
typedef gboolean (*BtRenameDbusFn) (const gchar *mac,
                                    const gchar *alias,
                                    GError     **error);
extern BtRenameDbusFn bt_rename_dbus_fn;
void bt_rename_set_dbus_fn (BtRenameDbusFn fn);

/* Synchronous rename: primary
 * bluetoothctl --timeout 5 set-alias MAC NOME, fallback to
 * bt_rename_dbus_fn on primary failure. Returns BT_RENAME_OK when
 * either path succeeds, primary classification otherwise. */
BtRenameStatus bt_rename_device (const gchar *mac,
                                const gchar *alias);

#endif /* QS_BT_BACKEND_H */
