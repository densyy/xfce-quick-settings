#include "qs-bt-signal.h"

#include <gio/gio.h>

struct BtSignalWatchPrivate
{
  GDBusConnection   *conn;
  guint              sub_id;
  BtSignalChangedCb  cb;
  gpointer           user_data;
};

static void
unref_pair (GVariant *a,
            GVariant *b)
{
  if (a != NULL)
    g_variant_unref (a);
  if (b != NULL)
    g_variant_unref (b);
}

static gboolean
iface_is_bluez_device (const gchar *iface)
{
  return g_strcmp0 (iface, "org.bluez.Device1") == 0;
}

static void
bt_signal_on_dbus_signal (GDBusConnection *connection,
                           const gchar     *sender_name,
                           const gchar     *object_path,
                           const gchar     *interface_name,
                           const gchar     *signal_name,
                           GVariant        *parameters,
                           gpointer         user_data)
{
  BtSignalWatch *watch = user_data;
  const gchar   *iface = NULL;
  GVariant      *changed = NULL;
  GVariant      *invalid = NULL;

  (void) connection;
  if (watch == NULL || watch->cb == NULL)
    return;
  if (g_strcmp0 (interface_name, "org.freedesktop.DBus.Properties") != 0)
    return;
  if (g_strcmp0 (signal_name, "PropertiesChanged") != 0)
    return;
  if (parameters == NULL)
    return;
  g_variant_get (parameters, "(&s@a{sv}@as)", &iface, &changed, &invalid);
  if (!iface_is_bluez_device (iface))
    {
      unref_pair (changed, invalid);
      return;
    }
  watch->cb (object_path, changed, sender_name, watch->user_data);
  unref_pair (changed, invalid);
}

BtSignalWatch *
bt_signal_subscribe (BtSignalChangedCb  cb,
                     gpointer           user_data,
                     GError            **error)
{
  GDBusConnection *conn;
  BtSignalWatch   *watch;
  guint            sub_id;

  if (cb == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "bt_signal_subscribe: callback is NULL");
      return NULL;
    }
  conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (conn == NULL)
    return NULL;
  watch = g_new0 (BtSignalWatch, 1);
  watch->conn = conn;
  watch->cb = cb;
  watch->user_data = user_data;
  sub_id = g_dbus_connection_signal_subscribe (conn,
                                               NULL,
                                               "org.freedesktop.DBus.Properties",
                                               "PropertiesChanged",
                                               NULL,
                                               "org.bluez.Device1",
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               bt_signal_on_dbus_signal,
                                               watch,
                                               NULL);
  if (sub_id == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "bt_signal_subscribe: failed to subscribe");
      g_object_unref (conn);
      g_free (watch);
      return NULL;
    }
  watch->sub_id = sub_id;
  return watch;
}

BtSignalWatch *
bt_signal_watch_new_for_test (BtSignalChangedCb cb,
                              gpointer          user_data)
{
  BtSignalWatch *watch = g_new0 (BtSignalWatch, 1);

  watch->cb = cb;
  watch->user_data = user_data;
  return watch;
}

BtSignalSubscribeFn bt_signal_subscribe_fn = bt_signal_subscribe;

void
bt_signal_set_subscribe_fn (BtSignalSubscribeFn fn)
{
  if (fn == NULL)
    bt_signal_subscribe_fn = bt_signal_subscribe;
  else
    bt_signal_subscribe_fn = fn;
}

void
bt_signal_test_emit (BtSignalWatch *watch,
                     const gchar   *object_path,
                     GVariant      *changed_properties,
                     const gchar   *sender_name)
{
  if (watch == NULL || watch->cb == NULL)
    return;
  if (object_path == NULL || changed_properties == NULL)
    return;
  watch->cb (object_path, changed_properties, sender_name, watch->user_data);
}

void
bt_signal_unsubscribe (BtSignalWatch *watch)
{
  if (watch == NULL)
    return;
  if (watch->sub_id != 0 && watch->conn != NULL)
    g_dbus_connection_signal_unsubscribe (watch->conn, watch->sub_id);
  watch->sub_id = 0;
  if (watch->conn != NULL)
    {
      g_object_unref (watch->conn);
      watch->conn = NULL;
    }
  g_free (watch);
}
