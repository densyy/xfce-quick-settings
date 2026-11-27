#include "qs-bt-agent.h"

#include <gio/gio.h>
#include <string.h>

struct QsBtAgent
{
  GDBusConnection *conn;
  guint            reg_id;
  gchar           *target;
  GtkWindow       *parent;
};

/* Interface org.bluez.Agent1 completa: o BlueZ pode chamar
 * qualquer metodo durante o pareamento, entao todos respondem.
 * Somente os pedidos Request* sao filtrados pelo alvo; os metodos
 * de exibicao e ciclo de vida apenas confirmam. */
static const gchar qs_bt_agent_xml[] =
  "<node>"
  "  <interface name=\"org.bluez.Agent1\">"
  "    <method name=\"Release\"/>"
  "    <method name=\"RequestPinCode\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"s\" name=\"pincode\" direction=\"out\"/>"
  "    </method>"
  "    <method name=\"DisplayPinCode\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"s\" name=\"pincode\" direction=\"in\"/>"
  "    </method>"
  "    <method name=\"RequestPasskey\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"u\" name=\"passkey\" direction=\"out\"/>"
  "    </method>"
  "    <method name=\"DisplayPasskey\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"u\" name=\"passkey\" direction=\"in\"/>"
  "      <arg type=\"u\" name=\"entered\" direction=\"in\"/>"
  "    </method>"
  "    <method name=\"RequestConfirmation\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"u\" name=\"passkey\" direction=\"in\"/>"
  "    </method>"
  "    <method name=\"RequestAuthorization\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "    </method>"
  "    <method name=\"AuthorizeService\">"
  "      <arg type=\"o\" name=\"device\" direction=\"in\"/>"
  "      <arg type=\"s\" name=\"uuid\" direction=\"in\"/>"
  "    </method>"
  "    <method name=\"Cancel\"/>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *qs_bt_agent_node_info = NULL;

static gboolean
bt_mac_char_valid (gsize i,
                   gchar c)
{
  if ((i % 3) == 2)
    return c == ':';
  return g_ascii_isxdigit (c) != 0;
}

static gchar
bt_mac_clean_char (gsize i,
                   gchar c)
{
  if ((i % 3) == 2)
    return '_';
  return g_ascii_toupper (c);
}

static const gchar *
path_basename (const gchar *path)
{
  const gchar *base = strrchr (path, '/');

  return (base != NULL) ? base + 1 : path;
}

gchar *
qs_bt_agent_device_path_for_mac (const gchar *mac)
{
  gchar  clean[18];
  gsize  i;
  gsize  pos = 0;

  if (mac == NULL)
    return NULL;
  if (strlen (mac) != 17)
    return NULL;
  for (i = 0; i < 17; i++)
    {
      if (!bt_mac_char_valid (i, mac[i]))
        return NULL;
      clean[pos++] = bt_mac_clean_char (i, mac[i]);
    }
  clean[pos] = '\0';
  return g_strconcat ("/org/bluez/hci0/dev_", clean, NULL);
}

gboolean
qs_bt_agent_path_matches (const gchar *target_path,
                          const gchar *request_path)
{
  const gchar *want;
  const gchar *base;

  if (target_path == NULL || *target_path == '\0')
    return FALSE;
  if (request_path == NULL || *request_path == '\0')
    return FALSE;
  want = path_basename (target_path);
  base = path_basename (request_path);
  if (g_ascii_strncasecmp (base, "dev_", 4) != 0)
    return FALSE;
  if (g_ascii_strncasecmp (want, "dev_", 4) != 0)
    return FALSE;
  return g_ascii_strcasecmp (base, want) == 0;
}

gboolean
qs_bt_agent_is_target (const QsBtAgent *agent,
                       const gchar     *request_path)
{
  if (agent == NULL || agent->target == NULL)
    return FALSE;
  return qs_bt_agent_path_matches (agent->target, request_path);
}

const gchar *
qs_bt_agent_get_target (const QsBtAgent *agent)
{
  if (agent == NULL)
    return NULL;
  return agent->target;
}

gchar *
qs_bt_agent_ask_pin (QsBtAgent *agent,
                     GtkWindow *parent)
{
  GtkWidget *dialog;
  GtkWidget *area;
  GtkWidget *label;
  GtkWidget *entry;
  GtkWindow *owner;
  gchar     *pin = NULL;
  gint       response;

  if (agent == NULL)
    return NULL;
  if (gdk_display_get_default () == NULL)
    return NULL;
  if (parent != NULL)
    owner = parent;
  else
    owner = agent->parent;
  dialog = gtk_dialog_new_with_buttons ("Bluetooth pairing",
                                        owner,
                                        GTK_DIALOG_MODAL
                                        | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                        "_Pair", GTK_RESPONSE_ACCEPT,
                                        NULL);
  area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  label = gtk_label_new ("Enter PIN code:");
  gtk_box_pack_start (GTK_BOX (area), label, FALSE, FALSE, 6);
  entry = gtk_entry_new ();
  gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
  gtk_entry_set_max_length (GTK_ENTRY (entry), 16);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_box_pack_start (GTK_BOX (area), entry, FALSE, FALSE, 6);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
  gtk_widget_show_all (dialog);
  response = gtk_dialog_run (GTK_DIALOG (dialog));
  if (response == GTK_RESPONSE_ACCEPT)
    {
      const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));

      if (text != NULL && *text != '\0')
        pin = g_strdup (text);
    }
  gtk_widget_destroy (dialog);
  return pin;
}

/* Erro padrao para pedido fora do alvo: o BlueZ espera
 * org.bluez.Error.Rejected para seguir com outro agente. */
static void
qs_bt_agent_reject (GDBusMethodInvocation *invocation,
                    const gchar           *detail)
{
  g_dbus_method_invocation_return_dbus_error (invocation,
                                              QS_BT_AGENT_REJECTED,
                                              detail);
}

/* Accept automatico para o alvo em metodos sem PIN; qualquer
 * outro caminho recebe Rejected sem dialogo. */
static void
qs_bt_agent_handle_confirm (QsBtAgent               *agent,
                            const gchar              *device,
                            GDBusMethodInvocation    *invocation,
                            const gchar              *method)
{
  gchar *detail;

  if (qs_bt_agent_is_target (agent, device))
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("()"));
      return;
    }
  detail = g_strdup_printf ("%s: not the pairing target", method);
  qs_bt_agent_reject (invocation, detail);
  g_free (detail);
}

static void
qs_bt_agent_handle_pincode (QsBtAgent               *agent,
                            GVariant                *parameters,
                            GDBusMethodInvocation   *invocation)
{
  const gchar *device = NULL;
  gchar       *pin;

  g_variant_get (parameters, "(&o)", &device);
  if (!qs_bt_agent_is_target (agent, device))
    {
      qs_bt_agent_reject (invocation, "RequestPinCode: not the pairing target");
      return;
    }
  pin = qs_bt_agent_ask_pin (agent, NULL);
  if (pin == NULL)
    {
      qs_bt_agent_reject (invocation, "RequestPinCode: cancelled");
      return;
    }
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(s)", pin));
  g_free (pin);
}

static void
qs_bt_agent_handle_passkey (QsBtAgent               *agent,
                             GVariant                *parameters,
                             GDBusMethodInvocation   *invocation)
{
  const gchar *device = NULL;
  gchar       *pin;
  gchar       *end = NULL;
  gulong       value;

  g_variant_get (parameters, "(&o)", &device);
  if (!qs_bt_agent_is_target (agent, device))
    {
      qs_bt_agent_reject (invocation, "RequestPasskey: not the pairing target");
      return;
    }
  pin = qs_bt_agent_ask_pin (agent, NULL);
  if (pin == NULL)
    {
      qs_bt_agent_reject (invocation, "RequestPasskey: cancelled");
      return;
    }
  value = g_ascii_strtoull (pin, &end, 10);
  g_free (pin);
  if (end == NULL || *end != '\0' || value > G_MAXUINT32)
    {
      qs_bt_agent_reject (invocation, "RequestPasskey: invalid passkey");
      return;
    }
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u)", (guint32) value));
}

static void
qs_bt_agent_handle_confirm_variant (QsBtAgent               *agent,
                                    GVariant                *parameters,
                                    GDBusMethodInvocation   *invocation,
                                    const gchar             *method_name,
                                    const gchar             *format)
{
  const gchar *device = NULL;

  g_variant_get (parameters, format, &device, NULL);
  qs_bt_agent_handle_confirm (agent, device, invocation, method_name);
}

static void
qs_bt_agent_method_call (GDBusConnection       *connection,
                         const gchar           *sender,
                         const gchar           *object_path,
                         const gchar           *interface_name,
                         const gchar           *method_name,
                         GVariant              *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer               user_data)
{
  QsBtAgent *agent = user_data;

  (void) connection;
  (void) sender;
  (void) object_path;
  if (g_strcmp0 (interface_name, "org.bluez.Agent1") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "unknown interface");
      return;
    }
  if (g_strcmp0 (method_name, "RequestPinCode") == 0)
    {
      qs_bt_agent_handle_pincode (agent, parameters, invocation);
      return;
    }
  if (g_strcmp0 (method_name, "RequestPasskey") == 0)
    {
      qs_bt_agent_handle_passkey (agent, parameters, invocation);
      return;
    }
  if (g_strcmp0 (method_name, "RequestConfirmation") == 0)
    {
      qs_bt_agent_handle_confirm_variant (agent, parameters, invocation,
                                          method_name, "(&ou)");
      return;
    }
  if (g_strcmp0 (method_name, "RequestAuthorization") == 0)
    {
      qs_bt_agent_handle_confirm_variant (agent, parameters, invocation,
                                          method_name, "(&o)");
      return;
    }
  if (g_strcmp0 (method_name, "AuthorizeService") == 0)
    {
      qs_bt_agent_handle_confirm_variant (agent, parameters, invocation,
                                          method_name, "(&o&s)");
      return;
    }
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
}

static const GDBusInterfaceVTable qs_bt_agent_vtable =
{
  qs_bt_agent_method_call,
  NULL,
  NULL,
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

QsBtAgent *
qs_bt_agent_register (const gchar  *mac,
                      GtkWindow    *parent,
                      GError      **error)
{
  QsBtAgent       *agent;
  GDBusConnection *conn;
  GError          *local = NULL;
  GVariant        *ret;

  agent = g_new0 (QsBtAgent, 1);
  agent->target = qs_bt_agent_device_path_for_mac (mac);
  if (agent->target == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "qs_bt_agent_register: invalid MAC");
      g_free (agent);
      return NULL;
    }
  agent->parent = parent;
  conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (conn == NULL)
    {
      g_free (agent->target);
      g_free (agent);
      return NULL;
    }
  agent->conn = conn;
  if (qs_bt_agent_node_info == NULL)
    {
      qs_bt_agent_node_info = g_dbus_node_info_new_for_xml (qs_bt_agent_xml,
                                                            &local);
      if (qs_bt_agent_node_info == NULL)
        {
          g_propagate_error (error, local);
          g_object_unref (conn);
          g_free (agent->target);
          g_free (agent);
          return NULL;
        }
    }
  agent->reg_id = g_dbus_connection_register_object (
    conn,
    QS_BT_AGENT_OBJECT_PATH,
    qs_bt_agent_node_info->interfaces[0],
    &qs_bt_agent_vtable,
    agent,
    NULL,
    &local);
  if (agent->reg_id == 0)
    {
      g_propagate_error (error, local);
      g_object_unref (conn);
      g_free (agent->target);
      g_free (agent);
      return NULL;
    }
  ret = g_dbus_connection_call_sync (conn,
                                     "org.bluez",
                                     "/org/bluez",
                                     "org.bluez.AgentManager1",
                                     "RegisterAgent",
                                     g_variant_new ("(os)",
                                                    QS_BT_AGENT_OBJECT_PATH,
                                                    QS_BT_AGENT_CAPABILITY),
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &local);
  if (ret == NULL)
    {
      g_propagate_error (error, local);
      g_dbus_connection_unregister_object (conn, agent->reg_id);
      g_object_unref (conn);
      g_free (agent->target);
      g_free (agent);
      return NULL;
    }
  g_variant_unref (ret);
  ret = g_dbus_connection_call_sync (conn,
                                     "org.bluez",
                                     "/org/bluez",
                                     "org.bluez.AgentManager1",
                                     "RequestDefaultAgent",
                                     g_variant_new ("(o)",
                                                    QS_BT_AGENT_OBJECT_PATH),
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &local);
  if (ret == NULL)
    {
      g_warning ("qs_bt_agent_register: RequestDefaultAgent failed: %s",
                 local->message);
      g_clear_error (&local);
    }
  else
    g_variant_unref (ret);
  return agent;
}

void
qs_bt_agent_unregister (QsBtAgent *agent)
{
  GVariant *ret;

  if (agent == NULL)
    return;
  if (agent->conn != NULL)
    {
      ret = g_dbus_connection_call_sync (agent->conn,
                                         "org.bluez",
                                         "/org/bluez",
                                         "org.bluez.AgentManager1",
                                         "UnregisterAgent",
                                         g_variant_new ("(o)",
                                                        QS_BT_AGENT_OBJECT_PATH),
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         NULL);
      if (ret != NULL)
        g_variant_unref (ret);
      if (agent->reg_id != 0)
        {
          g_dbus_connection_unregister_object (agent->conn, agent->reg_id);
          agent->reg_id = 0;
        }
      g_object_unref (agent->conn);
      agent->conn = NULL;
    }
  g_free (agent->target);
  agent->target = NULL;
  g_free (agent);
}
