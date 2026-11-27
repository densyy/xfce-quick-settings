#include "qs-core.h"
#include "qs-net.h"
#include "qs-ui.h"
#include "qs-util.h"
#include "qs-bt-backend.h"
#include "qs-bt-signal.h"

#include <string.h>

static void
bt_info_text_flags (const gchar *output,
                    gboolean    *paired,
                    gboolean    *trusted,
                    gboolean    *connected)
{
  if (paired != NULL)
    *paired = (output != NULL && strstr (output, "Paired: yes") != NULL);
  if (trusted != NULL)
    *trusted = (output != NULL && strstr (output, "Trusted: yes") != NULL);
  if (connected != NULL)
    *connected = (output != NULL && strstr (output, "Connected: yes") != NULL);
}

#define BT_CACHE_TTL_SECS (3 * 3600)

static GList   *bt_cache_devs = NULL;
static gchar   *bt_cache_controller = NULL;
static gboolean bt_cache_powered = FALSE;
static gint64   bt_cache_mtime = 0;

static BtDevice *
bt_device_copy (const BtDevice *src)
{
  BtDevice *dst = g_new0 (BtDevice, 1);

  dst->mac = g_strdup (src->mac);
  dst->name = g_strdup (src->name);
  dst->paired = src->paired;
  dst->trusted = src->trusted;
  dst->connected = src->connected;
  return dst;
}

static gboolean
bt_cache_valid (void)
{
  if (bt_cache_mtime == 0)
    return FALSE;
  return (g_get_monotonic_time () - bt_cache_mtime)
         < ((gint64) BT_CACHE_TTL_SECS * G_USEC_PER_SEC);
}

static void
bt_cache_store (const gchar *controller,
                gboolean     powered,
                GList       *devs)
{
  GList *l;
  GList *fresh = NULL;

  g_list_free_full (bt_cache_devs, bt_device_free);
  bt_cache_devs = NULL;
  for (l = devs; l != NULL; l = l->next)
    fresh = g_list_prepend (fresh, bt_device_copy (l->data));
  bt_cache_devs = g_list_reverse (fresh);
  g_free (bt_cache_controller);
  bt_cache_controller = g_strdup (controller);
  bt_cache_powered = powered;
  bt_cache_mtime = g_get_monotonic_time ();
}

static void
bt_cache_update (const gchar *mac,
                 gboolean     paired,
                 gboolean     trusted,
                 gboolean     connected,
                 gboolean     have_flags)
{
  GList *l;

  if (!have_flags || mac == NULL)
    return;
  for (l = bt_cache_devs; l != NULL; l = l->next)
    {
      BtDevice *dev = l->data;

      if (g_strcmp0 (dev->mac, mac) == 0)
        {
          dev->paired = paired;
          dev->trusted = trusted;
          dev->connected = connected;
          break;
        }
    }
}

/* Per-MAC single-flight with 30 s expiry (main loop only). */
#define BT_BUSY_TIMEOUT_SECS 30

static GHashTable *bt_busy = NULL; /* mac -> gint64* expiry (monotonic) */

static void
bt_busy_ensure (void)
{
  if (bt_busy == NULL)
    bt_busy = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     g_free, g_free);
}

static gboolean
bt_busy_expired (gint64 expiry)
{
  return g_get_monotonic_time () >= expiry;
}

static void
bt_busy_prune (void)
{
  GHashTableIter iter;
  gpointer       key;
  gpointer       value;

  if (bt_busy == NULL)
    return;
  g_hash_table_iter_init (&iter, bt_busy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      gint64 *expiry = value;

      if (expiry == NULL || bt_busy_expired (*expiry))
        g_hash_table_iter_remove (&iter);
    }
}

static gboolean
bt_is_busy_mac (const gchar *mac)
{
  gint64 *expiry;

  if (mac == NULL)
    return FALSE;
  bt_busy_ensure ();
  bt_busy_prune ();
  expiry = g_hash_table_lookup (bt_busy, mac);
  return (expiry != NULL && !bt_busy_expired (*expiry));
}

static void
bt_set_busy_mac (const gchar *mac)
{
  gint64 *expiry;

  if (mac == NULL)
    return;
  bt_busy_ensure ();
  expiry = g_new (gint64, 1);
  *expiry = g_get_monotonic_time ()
            + (gint64) BT_BUSY_TIMEOUT_SECS * G_USEC_PER_SEC;
  g_hash_table_replace (bt_busy, g_strdup (mac), expiry);
}

static void
bt_clear_busy_mac (const gchar *mac)
{
  if (mac == NULL || bt_busy == NULL)
    return;
  g_hash_table_remove (bt_busy, mac);
}

static void
bt_busy_clear_all (void)
{
  if (bt_busy == NULL)
    return;
  g_hash_table_remove_all (bt_busy);
}

void
bt_free_caches (void)
{
  g_list_free_full (bt_cache_devs, bt_device_free);
  bt_cache_devs = NULL;
  g_free (bt_cache_controller);
  bt_cache_controller = NULL;
  bt_cache_powered = FALSE;
  bt_cache_mtime = 0;
  bt_busy_clear_all ();
  if (bt_busy != NULL)
    {
      g_hash_table_destroy (bt_busy);
      bt_busy = NULL;
    }
}

static gboolean bt_filling = FALSE;

static void
bt_style_card_row (GtkWidget *row,
                   GtkWidget *content)
{
  ui_add_class (row, "qs-card");
  gtk_widget_set_margin_top (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_bottom (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_start (row, QS_TIGHT_GAP);
  gtk_widget_set_margin_end (row, QS_TIGHT_GAP);
  gtk_container_set_border_width (GTK_CONTAINER (content), QS_CONTENT_PAD);
}

static const gchar *
bt_state_text (gboolean connected,
               gboolean paired)
{
  if (connected)
    return "Connected";
  return paired ? "Paired" : "";
}

static GtkWidget *
bt_row_new (const BtDevice *dev)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *icon = gtk_image_new_from_icon_name (dev->connected
                     ? "bluetooth-active-symbolic"
                     : "bluetooth-symbolic",
                     GTK_ICON_SIZE_LARGE_TOOLBAR);
  GtkWidget *name = gtk_label_new (dev->name);
  GtkWidget *state = gtk_label_new (bt_state_text (dev->connected, dev->paired));

  gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
  ui_add_class (name, "qs-title");
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  ui_add_class (state, "qs-sub");
  gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), name, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), state, FALSE, FALSE, 0);
  bt_style_card_row (row, hbox);
  gtk_container_add (GTK_CONTAINER (row), hbox);
  g_object_set_data_full (G_OBJECT (row), "qs-mac",
                          g_strdup (dev->mac), g_free);
  g_object_set_data_full (G_OBJECT (row), "qs-name",
                          g_strdup (dev->name), g_free);
  g_object_set_data (G_OBJECT (row), "qs-connected",
                     GINT_TO_POINTER (dev->connected));
  g_object_set_data (G_OBJECT (row), "qs-paired",
                     GINT_TO_POINTER (dev->paired));
  return row;
}

static void
bt_sync_sub (QuickSettings *qs,
             const gchar   *connected_name,
             gboolean       have_devs)
{
  g_free (qs->bt_connected_name);
  if (connected_name != NULL)
    {
      qs->bt_connected_name = g_strdup (connected_name);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), connected_name);
      return;
    }
  qs->bt_connected_name = NULL;
  if (have_devs)
    gtk_label_set_text (GTK_LABEL (qs->bt_sub), "On");
}

static void
bt_fill_list (QuickSettings *qs,
              GList         *devs,
              const gchar   *empty_text)
{
  GList *l;
  const gchar *connected_name = NULL;

  bt_filling = TRUE;
  clear_list_box (qs->bt_list);
  if (devs == NULL)
    gtk_container_add (GTK_CONTAINER (qs->bt_list),
                       make_placeholder_row (empty_text));
  for (l = devs; l != NULL; l = l->next)
    {
      BtDevice *dev = l->data;

      if (dev->connected)
        connected_name = dev->name;
      gtk_container_add (GTK_CONTAINER (qs->bt_list), bt_row_new (dev));
    }
  bt_sync_sub (qs, connected_name, devs != NULL);
  gtk_widget_show_all (qs->bt_list);
  bt_filling = FALSE;
}

static void
bt_sync_switch_tile (QuickSettings *qs)
{
  qs->updating = TRUE;
  gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), qs->bt_on);
  qs->updating = FALSE;
  set_tile_active (qs->bt_tile, qs->bt_on);
}

static void
bt_paint_state (QuickSettings *qs,
                gboolean       powered,
                GList         *devs)
{
  if (qs->bt_controller == NULL)
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status),
                          command_exists ("bluetoothctl")
                          ? "No Bluetooth adapter found."
                          : "Install bluez-utils.");
      gtk_widget_set_sensitive (qs->bt_switch, FALSE);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), "No adapter");
      bt_fill_list (qs, NULL, "No adapter.");
    }
  else if (!powered)
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "Bluetooth is off.");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      gtk_label_set_text (GTK_LABEL (qs->bt_sub), "Off");
      bt_fill_list (qs, NULL, "Turn Bluetooth on to see devices.");
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (qs->bt_status), "");
      gtk_widget_set_sensitive (qs->bt_switch, TRUE);
      bt_fill_list (qs, devs, "No devices found.");
    }
}

static GtkWidget *
bt_find_row (QuickSettings *qs,
             const gchar   *mac)
{
  GList *rows = gtk_container_get_children (GTK_CONTAINER (qs->bt_list));
  GList *l;
  GtkWidget *found = NULL;

  for (l = rows; l != NULL; l = l->next)
    {
      const gchar *m;

      if (!GTK_IS_LIST_BOX_ROW (l->data))
        continue;
      m = g_object_get_data (G_OBJECT (l->data), "qs-mac");
      if (g_strcmp0 (m, mac) == 0)
        {
          found = GTK_WIDGET (l->data);
          break;
        }
    }
  g_list_free (rows);
  return found;
}

static gboolean
bt_update_row (QuickSettings *qs,
               const gchar   *mac,
               gboolean       connected,
               gboolean       paired)
{
  GtkWidget *row = bt_find_row (qs, mac);
  GtkWidget *hbox;
  GList     *kids;
  GList     *k;
  GtkWidget *icon = NULL;
  GtkWidget *state = NULL;
  gint       labels_seen = 0;

  if (row == NULL)
    return FALSE;
  hbox = gtk_bin_get_child (GTK_BIN (row));
  if (hbox == NULL || !GTK_IS_BOX (hbox))
    return FALSE;
  kids = gtk_container_get_children (GTK_CONTAINER (hbox));
  for (k = kids; k != NULL; k = k->next)
    {
      if (GTK_IS_IMAGE (k->data) && icon == NULL)
        icon = GTK_WIDGET (k->data);
      else if (GTK_IS_LABEL (k->data))
        {
          labels_seen++;
          if (labels_seen == 2)
            state = GTK_WIDGET (k->data);
        }
    }
  if (state != NULL)
    gtk_label_set_text (GTK_LABEL (state),
                        connected ? "Connected"
                        : paired ? "Paired" : "");
  if (icon != NULL)
    gtk_image_set_from_icon_name (GTK_IMAGE (icon),
                                  connected
                                  ? "bluetooth-active-symbolic"
                                  : "bluetooth-symbolic",
                                  GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_list_free (kids);
  g_object_set_data (G_OBJECT (row), "qs-connected",
                     GINT_TO_POINTER (connected));
  g_object_set_data (G_OBJECT (row), "qs-paired",
                     GINT_TO_POINTER (paired));
  if (connected)
    {
      const gchar *nm = g_object_get_data (G_OBJECT (row), "qs-name");

      if (nm != NULL)
        {
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), nm);
          g_free (qs->bt_connected_name);
          qs->bt_connected_name = g_strdup (nm);
        }
    }
  else
    {
      const gchar *nm = g_object_get_data (G_OBJECT (row), "qs-name");
      const gchar *sub = gtk_label_get_text (GTK_LABEL (qs->bt_sub));

      if (nm != NULL && sub != NULL && g_strcmp0 (nm, sub) == 0)
        {
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "On");
          g_free (qs->bt_connected_name);
          qs->bt_connected_name = NULL;
        }
    }
  return TRUE;
}

typedef struct
{
  QuickSettings *qs;
  GList         *devs;
  gchar         *controller; /* owned, transfer to qs */
  gboolean       powered;
} BtResult;

static gboolean bt_load_idle (gpointer data);

static gpointer
bt_load_thread (gpointer data)
{
  BtResult *r = data;

  if (g_atomic_int_get (&r->qs->dead))
    {
      g_idle_add (bt_load_idle, r);
      return NULL;
    }
  r->controller = bt_get_controller ();
  if (r->controller != NULL)
    r->powered = bt_powered (r->controller);
  if (r->powered && !g_atomic_int_get (&r->qs->dead))
    r->devs = list_bt_devices ();
  g_idle_add (bt_load_idle, r);
  return NULL;
}

static gboolean
bt_load_idle (gpointer data)
{
  BtResult      *r = data;
  QuickSettings *qs = r->qs;

  if (g_atomic_int_get (&qs->dead))
    {
      g_list_free_full (r->devs, bt_device_free);
      g_free (r->controller);
      g_free (r);
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }

  g_free (qs->bt_controller);
  qs->bt_controller = r->controller;
  r->controller = NULL;
  qs->bt_on = (qs->bt_controller != NULL && r->powered);
  bt_sync_switch_tile (qs);
  bt_paint_state (qs, r->powered, r->devs);

  bt_cache_store (qs->bt_controller, r->powered, r->devs);
  qs->scanning_bt = FALSE;
  gtk_widget_set_sensitive (qs->bt_scan, TRUE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scan for devices");

  g_list_free_full (r->devs, bt_device_free);
  g_free (r->controller);
  g_free (r);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static gboolean
bt_spawn_named (const gchar *name,
                GThreadFunc  func,
                gpointer     data)
{
  GThread *t = g_thread_try_new (name, func, data, NULL);

  if (t == NULL)
    return FALSE;
  g_thread_unref (t);
  return TRUE;
}

static void
bt_start_load_thread (QuickSettings *qs)
{
  BtResult *r = g_new0 (BtResult, 1);

  r->qs = qs;
  qs_ref (qs);
  if (!bt_spawn_named ("qs-bt", bt_load_thread, r))
    {
      qs_unref (qs);
      g_free (r);
    }
}

static void
bt_scan_done (const gchar *output,
              const gchar *errout,
              gboolean     ok,
              gpointer     user_data)
{
  QuickSettings *qs = user_data;

  (void) output;
  (void) errout;
  (void) ok;
  if (g_atomic_int_get (&qs->dead))
    {
      qs_unref (qs);
      return;
    }
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Reading devices…");
  bt_start_load_thread (qs);
  qs_unref (qs);
}

static void
on_bt_scan_clicked (GtkButton *button,
                    gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  if (qs->scanning_bt)
    return;
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scanning…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Scanning for devices…");
  /* Discovery stops by itself when bluetoothctl exits after the timeout. */
  qs_ref (qs);
  run_command_async ("bluetoothctl --timeout 6 scan on", bt_scan_done, qs);
}

static gboolean
bt_reload_timeout (gpointer data)
{
  QuickSettings *qs = data;

  if (g_atomic_int_get (&qs->dead))
    {
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  if (qs->scanning_bt)
    {
      qs_unref (qs);
      return G_SOURCE_REMOVE;
    }
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  bt_start_load_thread (qs);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static void
on_bt_switch_changed (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  QuickSettings *qs = user_data;
  gboolean       on = gtk_switch_get_active (GTK_SWITCH (obj));

  (void) pspec;
  if (qs->updating)
    return;
  qs->bt_on = on;
  set_tile_active (qs->bt_tile, on);
  bt_set_powered (on);
  gtk_label_set_text (GTK_LABEL (qs->bt_status),
                      on ? "Turning Bluetooth on…"
                         : "Turning Bluetooth off…");
  qs_ref (qs);
  g_timeout_add_seconds (1, bt_reload_timeout, qs);
}

typedef struct
{
  QuickSettings *qs;
  gchar         *mac;
  gchar         *name;
  gchar        **verbs; /* remaining verbs, NULL-terminated (owned) */
  gint           idx;
  gboolean       disconnecting;
  gboolean       expected_connected; /* Connected value the link waits for */
  gboolean       initial_connected; /* Connected from the toggle decision info */
  gboolean       initial_paired; /* Paired from the toggle decision info */
  gboolean       initial_trusted; /* Trusted from the toggle decision info */
  BtSignalWatch *watch; /* GDBus subscription, NULL when inactive */
  guint          fallback_src; /* pending fallback timeout (0 when none) */
  gchar         *verb_error; /* error detail of the first failed verb (owned) */
  gint           pending; /* outstanding async commands (verbs + confirm read) */
  gboolean       finished; /* completion done, draining pending callbacks */
  gboolean       cancelled; /* set by bt_cancel_all() / destroy path */
} BtLink;

static GList *bt_active_links = NULL;

static void
bt_link_track (BtLink *l)
{
  bt_active_links = g_list_prepend (bt_active_links, l);
}

static void
bt_link_untrack (BtLink *l)
{
  bt_active_links = g_list_remove (bt_active_links, l);
}

#define BT_LINK_CTL_TIMEOUT_SECS 15
#define BT_LINK_CMD_TIMEOUT_SECS 20

#define BT_SIGNAL_FALLBACK_TIMEOUT_SECS 8

static guint
bt_link_timeout_secs (void)
{
  const gchar *env = g_getenv ("QS_BT_LINK_TIMEOUT_SECS");
  gint         value;

  if (env == NULL || !qs_parse_int_checked (env, &value) || value <= 0)
    return BT_LINK_CMD_TIMEOUT_SECS;
  return (guint) value;
}

static guint
bt_signal_timeout_secs (void)
{
  const gchar *env = g_getenv ("QS_BT_SIGNAL_TIMEOUT_SECS");
  gint         value;

  if (env == NULL || !qs_parse_int_checked (env, &value) || value <= 0)
    return BT_SIGNAL_FALLBACK_TIMEOUT_SECS;
  return (guint) value;
}

static void
bt_link_unsubscribe (BtLink *l)
{
  if (l->watch != NULL)
    {
      bt_signal_unsubscribe (l->watch);
      l->watch = NULL;
    }
}

static void
bt_link_remove_fallback (BtLink *l)
{
  if (l->fallback_src != 0)
    {
      g_source_remove (l->fallback_src);
      l->fallback_src = 0;
    }
}

static void
bt_link_free (BtLink *l)
{
  if (l == NULL)
    return;
  bt_link_unsubscribe (l);
  bt_link_remove_fallback (l);
  bt_link_untrack (l);
  g_free (l->mac);
  g_free (l->name);
  g_free (l->verb_error);
  g_strfreev (l->verbs);
  qs_unref (l->qs);
  g_free (l);
}

static void
bt_link_try_free (BtLink *l)
{
  if (l->finished && l->pending <= 0)
    bt_link_free (l);
}

/* Marks the link complete without touching widgets (cancel/dead path). */
static void
bt_link_drop (BtLink *l)
{
  gchar *copy;

  if (l->finished)
    return;
  l->finished = TRUE;
  bt_link_unsubscribe (l);
  bt_link_remove_fallback (l);
  copy = (l->mac != NULL) ? g_strdup (l->mac) : NULL;
  bt_clear_busy_mac (copy);
  g_free (copy);
  bt_link_try_free (l);
}

/* Cancels pending links; next callbacks drop without touching widgets. */
void
bt_cancel_all (void)
{
  GList *copy = g_list_copy (bt_active_links);
  GList *l;

  for (l = copy; l != NULL; l = l->next)
    {
      BtLink *link = l->data;

      link->cancelled = TRUE;
      bt_link_unsubscribe (link);
      bt_link_remove_fallback (link);
      bt_link_drop (link);
    }
  g_list_free (copy);
}

/* First non-empty line of output/errout for status details (owned/NULL). */
static gchar *
bt_first_detail (const gchar *output,
                 const gchar *errout)
{
  const gchar *src = (output != NULL && *output != '\0') ? output : errout;
  gchar       *copy;
  gchar       *nl;
  gchar       *stripped;

  if (src == NULL || *src == '\0')
    return NULL;
  copy = g_strdup (src);
  nl = strchr (copy, '\n');
  if (nl != NULL)
    *nl = '\0';
  stripped = g_strstrip (copy);
  if (*stripped == '\0')
    {
      g_free (copy);
      return NULL;
    }
  memmove (copy, stripped, strlen (stripped) + 1);
  return copy;
}

/* Paired/trusted flags for a link success. A connected device is paired
 * by definition; otherwise the cached values are preserved. */
static void
bt_link_cached_flags (const gchar *mac,
                      gboolean       connected,
                      gboolean      *paired,
                      gboolean      *trusted)
{
  GList *l;

  if (paired != NULL)
    *paired = TRUE;
  if (trusted != NULL)
    *trusted = FALSE;
  for (l = bt_cache_devs; l != NULL; l = l->next)
    {
      BtDevice *dev = l->data;

      if (dev != NULL && g_strcmp0 (dev->mac, mac) == 0)
        {
          if (!connected && paired != NULL)
            *paired = dev->paired;
          if (trusted != NULL)
            *trusted = dev->trusted;
          break;
        }
    }
}

static void
bt_link_succeed (BtLink  *l,
                 gboolean paired,
                 gboolean trusted,
                 gboolean connected)
{
  gchar *msg;
  gchar *copy;

  if (l->finished)
    return;
  l->finished = TRUE;
  bt_link_unsubscribe (l);
  bt_link_remove_fallback (l);
  if (!g_atomic_int_get (&l->qs->dead) && !l->cancelled)
    {
      msg = g_strdup_printf (connected ? "Connected to %s."
                                       : "Disconnected from %s.",
                             l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
      bt_cache_update (l->mac, paired, trusted, connected, TRUE);
      if (!bt_update_row (l->qs, l->mac, connected, paired))
        bt_fill_list (l->qs, bt_cache_devs, "No devices found.");
    }
  copy = (l->mac != NULL) ? g_strdup (l->mac) : NULL;
  bt_clear_busy_mac (copy);
  g_free (copy);
  bt_link_try_free (l);
}

static void
bt_link_fail (BtLink      *l,
              const gchar *verb,
              const gchar *detail,
              gboolean     reload)
{
  gchar *msg;
  gchar *copy;

  if (l->finished)
    return;
  l->finished = TRUE;
  bt_link_unsubscribe (l);
  bt_link_remove_fallback (l);
  if (!g_atomic_int_get (&l->qs->dead) && !l->cancelled)
    {
      if (reload)
        bt_start_load_thread (l->qs);
      if (detail != NULL && *detail != '\0')
        msg = g_strdup_printf ("Failed to %s %s: %s", verb, l->name,
                               detail);
      else
        msg = g_strdup_printf ("Failed to %s %s.", verb, l->name);
      gtk_label_set_text (GTK_LABEL (l->qs->bt_status), msg);
      g_free (msg);
    }
  copy = (l->mac != NULL) ? g_strdup (l->mac) : NULL;
  bt_clear_busy_mac (copy);
  g_free (copy);
  bt_link_try_free (l);
}

static void bt_link_run_next (BtLink *l);
static gboolean bt_signal_fallback_timeout (gpointer data);
static void bt_fallback_info_done (const gchar *output,
                                   const gchar *errout,
                                   gboolean     ok,
                                   gpointer     user_data);

/* Arms the single-shot fallback after the last verb ends. The fallback
 * closes the link when no signal arrives; it never polls. */
static void
bt_link_arm_fallback (BtLink *l)
{
  if (l->finished || l->cancelled || l->fallback_src != 0)
    return;
  l->fallback_src = g_timeout_add_seconds (bt_signal_timeout_secs (),
                                           bt_signal_fallback_timeout, l);
}

/* Fallback when no signal arrives: at most one `info` confirmation read,
 * then success or failure with the verb detail. Never loops. */
static gboolean
bt_signal_fallback_timeout (gpointer data)
{
  BtLink *l = data;
  gchar  *cmd;
  gchar  *mac_q;

  l->fallback_src = 0;
  if (l->finished || l->cancelled || g_atomic_int_get (&l->qs->dead))
    {
      bt_link_drop (l);
      return G_SOURCE_REMOVE;
    }
  mac_q = g_shell_quote (l->mac);
  cmd = g_strdup_printf ("bluetoothctl --timeout 1 info %s", mac_q);
  g_free (mac_q);
  l->pending++;
  run_command_async (cmd, bt_fallback_info_done, l);
  g_free (cmd);
  return G_SOURCE_REMOVE;
}

/* GDBus event handler: only a value matching the expected Connected
 * state closes the link. Opposite or foreign events are ignored. */
static void
bt_signal_connected_changed (const gchar *object_path,
                             GVariant    *changed_properties,
                             const gchar *sender_name,
                             gpointer     user_data)
{
  BtLink       *l = user_data;
  BtSignalMatch match;

  (void) sender_name;
  if (l == NULL || l->finished || l->cancelled
      || g_atomic_int_get (&l->qs->dead))
    {
      if (l != NULL)
        bt_link_drop (l);
      return;
    }
  match = bt_signal_event_matches (object_path, l->mac, changed_properties);
  if (l->expected_connected && match == BT_SIGNAL_CONNECTED_TRUE)
    {
      gboolean paired = TRUE;
      gboolean trusted = FALSE;

      bt_link_cached_flags (l->mac, TRUE, &paired, &trusted);
      bt_link_succeed (l, paired, trusted, TRUE);
    }
  else if (!l->expected_connected && match == BT_SIGNAL_CONNECTED_FALSE)
    {
      gboolean paired = TRUE;
      gboolean trusted = FALSE;

      bt_link_cached_flags (l->mac, FALSE, &paired, &trusted);
      bt_link_succeed (l, paired, trusted, FALSE);
    }
}

/* Subscribes before the first verb spawn and checks the current value.
 * When the current value already matches the expected one, the link
 * completes without spawning any link verb. */
static gboolean
bt_link_ensure_signal (BtLink *l)
{
  GError *error = NULL;

  if (l->watch != NULL)
    return TRUE;
  l->expected_connected = !l->disconnecting;
  gtk_label_set_text (GTK_LABEL (l->qs->bt_status),
                      l->disconnecting ? "Disconnecting…" : "Connecting…");
  l->watch = bt_signal_subscribe_fn (bt_signal_connected_changed, l,
                                     &error);
  if (error != NULL)
    g_error_free (error);
  if (l->initial_connected == l->expected_connected)
    {
      bt_link_succeed (l, l->initial_paired, l->initial_trusted,
                       l->initial_connected);
      return FALSE;
    }
  return TRUE;
}

static void
bt_fallback_info_done (const gchar *output,
                       const gchar *errout,
                       gboolean     ok,
                       gpointer     user_data)
{
  BtLink      *l = user_data;
  gboolean     paired = FALSE;
  gboolean     trusted = FALSE;
  gboolean     connected = FALSE;
  const gchar *verb;

  (void) ok;
  l->pending--;
  if (l->finished || l->cancelled || g_atomic_int_get (&l->qs->dead))
    {
      bt_link_drop (l);
      return;
    }
  bt_info_text_flags (output, &paired, &trusted, &connected);
  if (connected == l->expected_connected)
    {
      bt_link_succeed (l, paired, trusted, connected);
      return;
    }
  verb = l->disconnecting ? "disconnect" : "connect";
  if (l->verb_error != NULL)
    {
      gchar *detail = l->verb_error;

      l->verb_error = NULL;
      bt_link_fail (l, verb, detail, TRUE);
      g_free (detail);
    }
  else
    {
      gchar *detail = bt_first_detail (output, errout);

      bt_link_fail (l, verb, detail, TRUE);
      g_free (detail);
    }
}

static void
bt_link_done (const gchar *output,
              const gchar *errout,
              gboolean     ok,
              gpointer     user_data)
{
  BtLink      *l = user_data;
  const gchar *verb;
  gint         count = 0;

  l->pending--;
  if (l->finished || l->cancelled || g_atomic_int_get (&l->qs->dead))
    {
      bt_link_drop (l);
      return;
    }
  verb = (l->verbs != NULL) ? l->verbs[l->idx] : "connect";
  if (verb == NULL)
    verb = l->disconnecting ? "disconnect" : "connect";
  if (!ok || bt_output_is_error (output, errout))
    {
      /* A failed verb may still connect in the background: keep the
       * subscription until the event or the fallback. */
      if (l->verb_error == NULL)
        l->verb_error = bt_first_detail (output, errout);
      if (l->verbs != NULL)
        while (l->verbs[count] != NULL)
          count++;
      l->idx = count;
      bt_link_arm_fallback (l);
      return;
    }
  l->idx++;
  bt_link_run_next (l);
}

static void
bt_link_run_next (BtLink *l)
{
  gchar *cmd;

  if (l->finished || l->cancelled || g_atomic_int_get (&l->qs->dead))
    {
      bt_link_drop (l);
      return;
    }
  if (l->verbs == NULL || l->verbs[l->idx] == NULL)
    {
      /* All verbs done: the event or the fallback closes the link. */
      bt_link_arm_fallback (l);
      return;
    }
  if (!bt_link_ensure_signal (l))
    return;
  {
    gchar *mac_q = g_shell_quote (l->mac);

    cmd = g_strdup_printf ("bluetoothctl --timeout %d %s %s",
                           BT_LINK_CTL_TIMEOUT_SECS, l->verbs[l->idx],
                           mac_q);
    g_free (mac_q);
  }
  l->pending++;
  run_command_async_timeout (cmd, bt_link_done, l, bt_link_timeout_secs ());
  g_free (cmd);
}

/* Connected devices disconnect; others connect (skipping pair/trust when done). */
static void
bt_toggle_info_done (const gchar *output,
                     const gchar *errout,
                     gboolean     ok,
                     gpointer     user_data)
{
  BtLink  *l = user_data;
  gboolean paired = FALSE;
  gboolean trusted = FALSE;
  gboolean connected = FALSE;

  (void) errout;
  (void) ok;
  if (g_atomic_int_get (&l->qs->dead) || l->cancelled)
    {
      bt_link_drop (l);
      return;
    }
  if (output == NULL)
    {
      bt_link_fail (l, "read", "could not query device info", FALSE);
      return;
    }
  bt_info_text_flags (output, &paired, &trusted, &connected);
  l->initial_connected = connected;
  l->initial_paired = paired;
  l->initial_trusted = trusted;
  if (connected)
    {
      l->disconnecting = TRUE;
      l->verbs = g_new0 (gchar *, 2);
      l->verbs[0] = g_strdup ("disconnect");
      l->idx = 0;
      bt_link_run_next (l);
    }
  else
    {
      GPtrArray *arr = g_ptr_array_new ();

      if (!paired)
        g_ptr_array_add (arr, g_strdup ("pair"));
      if (!trusted)
        g_ptr_array_add (arr, g_strdup ("trust"));
      g_ptr_array_add (arr, g_strdup ("connect"));
      g_ptr_array_add (arr, NULL);
      l->disconnecting = FALSE;
      l->verbs = (gchar **) g_ptr_array_free (arr, FALSE);
      l->idx = 0;
      bt_link_run_next (l);
    }
}

static void
bt_toggle_device (QuickSettings *qs,
                  GtkListBoxRow *row)
{
  const gchar *mac;
  const gchar *name;
  BtLink      *l;
  gchar       *cmd;

  if (row == NULL || !gtk_widget_get_sensitive (GTK_WIDGET (row)))
    return;
  if (bt_filling || g_atomic_int_get (&qs->dead))
    return;
  mac = g_object_get_data (G_OBJECT (row), "qs-mac");
  name = g_object_get_data (G_OBJECT (row), "qs-name");
  if (mac == NULL)
    return;
  if (bt_is_busy_mac (mac))
    return;
  l = g_new0 (BtLink, 1);
  l->qs = qs;
  l->mac = g_strdup (mac);
  l->name = g_strdup (name != NULL ? name : mac);
  l->verbs = NULL;
  l->idx = 0;
  l->disconnecting = FALSE;
  l->expected_connected = FALSE;
  l->initial_connected = FALSE;
  l->initial_paired = FALSE;
  l->initial_trusted = FALSE;
  l->watch = NULL;
  l->fallback_src = 0;
  l->verb_error = NULL;
  l->pending = 0;
  l->finished = FALSE;
  l->cancelled = FALSE;
  bt_set_busy_mac (mac);
  bt_link_track (l);
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Reading device…");
  {
    gchar *mac_q = g_shell_quote (l->mac);

    cmd = g_strdup_printf ("bluetoothctl --timeout 1 info %s", mac_q);
    g_free (mac_q);
  }
  run_command_async (cmd, bt_toggle_info_done, l);
  g_free (cmd);
}

static void
on_bt_row_activated (GtkListBox    *box,
                     GtkListBoxRow *row,
                     gpointer       user_data)
{
  (void) box;
  bt_toggle_device ((QuickSettings *) user_data, row);
}

static void
on_bt_row_selected (GtkListBox    *box,
                    GtkListBoxRow *row,
                    gpointer       user_data)
{
  (void) box;
  /* Single click starts the toggle; a second activation on a connected
   * device therefore disconnects. Guarded by bt_filling/per-MAC busy. */
  bt_toggle_device ((QuickSettings *) user_data, row);
}

/* Module-public (called by the qs-net facade). */

void
bt_show_page (QuickSettings *qs)
{
  if (!command_exists ("bluetoothctl"))
    return;
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "bt");
  if (qs->scanning_bt)
    return;
  if (bt_cache_valid ())
    {
      /* Reuse the in-memory cache; only Scan fetches new devices. */
      g_free (qs->bt_controller);
      qs->bt_controller = g_strdup (bt_cache_controller);
      qs->bt_on = (qs->bt_controller != NULL && bt_cache_powered);
      bt_sync_switch_tile (qs);
      bt_paint_state (qs, bt_cache_powered, bt_cache_devs);
      gtk_widget_set_sensitive (qs->bt_scan, TRUE);
      gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Scan for devices");
      return;
    }
  qs->scanning_bt = TRUE;
  gtk_widget_set_sensitive (qs->bt_scan, FALSE);
  gtk_button_set_label (GTK_BUTTON (qs->bt_scan), "Loading…");
  gtk_label_set_text (GTK_LABEL (qs->bt_status), "Loading…");
  bt_start_load_thread (qs);
}

void
bt_update_tile_cached (QuickSettings *qs)
{
  /* Never spawns: shows the last known state (see qs-net facade note).
   * The async probe corrects it moments after popup open. */
  set_tile_active (qs->bt_tile, qs->bt_on);
  gtk_widget_set_sensitive (qs->bt_tile, command_exists ("bluetoothctl"));
}

typedef struct
{
  QuickSettings *qs;
  gchar         *controller; /* owned MAC or NULL */
  gchar         *connected_name;
  gboolean       powered;
} BtTileProbe;

static gboolean
bt_tile_probe_idle (gpointer data)
{
  BtTileProbe   *p = data;
  QuickSettings *qs = p->qs;

  qs->bt_tile_probe = FALSE;
  /* A full page load owns the widgets while it runs; it updates the
   * tile itself when done. */
  if (!g_atomic_int_get (&qs->dead) && !qs->scanning_bt)
    {
      g_free (qs->bt_controller);
      qs->bt_controller = p->controller;
      p->controller = NULL;
      qs->bt_on = (qs->bt_controller != NULL && p->powered);
      qs->updating = TRUE;
      gtk_switch_set_active (GTK_SWITCH (qs->bt_switch), qs->bt_on);
      qs->updating = FALSE;
      set_tile_active (qs->bt_tile, qs->bt_on);

      g_free (qs->bt_connected_name);
      if (qs->bt_controller == NULL)
        {
          qs->bt_connected_name = NULL;
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "No adapter");
        }
      else if (!qs->bt_on)
        {
          qs->bt_connected_name = NULL;
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "Off");
        }
      else if (p->connected_name != NULL)
        {
          qs->bt_connected_name = g_strdup (p->connected_name);
          gtk_label_set_text (GTK_LABEL (qs->bt_sub),
                              qs->bt_connected_name);
        }
      else
        {
          qs->bt_connected_name = NULL;
          gtk_label_set_text (GTK_LABEL (qs->bt_sub), "On");
        }
    }
  g_free (p->controller);
  g_free (p->connected_name);
  g_free (p);
  qs_unref (qs);
  return G_SOURCE_REMOVE;
}

static gpointer
bt_tile_probe_thread (gpointer data)
{
  BtTileProbe *p = data;

  if (g_atomic_int_get (&p->qs->dead))
    {
      g_idle_add (bt_tile_probe_idle, p);
      return NULL;
    }
  p->controller = bt_get_controller ();
  if (p->controller != NULL && !g_atomic_int_get (&p->qs->dead))
    {
      p->powered = bt_powered (p->controller);
      if (p->powered && !g_atomic_int_get (&p->qs->dead))
        {
          GList *devs = list_bt_devices ();
          GList *l;

          for (l = devs; l != NULL; l = l->next)
            {
              BtDevice *dev = l->data;

              if (dev != NULL && dev->connected
                  && dev->name != NULL)
                {
                  p->connected_name = g_strdup (dev->name);
                  break;
                }
            }
          g_list_free_full (devs, bt_device_free);
        }
    }
  g_idle_add (bt_tile_probe_idle, p);
  return NULL;
}

void
bt_refresh_tile_async (QuickSettings *qs)
{
  BtTileProbe *p;

  if (!command_exists ("bluetoothctl"))
    return;
  if (qs->bt_tile_probe || qs->scanning_bt)
    return;
  qs->bt_tile_probe = TRUE;
  p = g_new0 (BtTileProbe, 1);
  p->qs = qs;
  qs_ref (qs);
  if (!bt_spawn_named ("qs-bt-tile", bt_tile_probe_thread, p))
    {
      qs->bt_tile_probe = FALSE;
      qs_unref (qs);
      g_free (p);
    }
}

void
bt_build_page (GtkWidget     *stack,
               QuickSettings *qs,
               GtkWidget    **back_out)
{
  GtkWidget *page = build_device_page ("Bluetooth", "Scan for devices",
                                       &qs->bt_switch, &qs->bt_scan,
                                       &qs->bt_list, &qs->bt_status,
                                       back_out);

  bt_busy_ensure ();
  gtk_stack_add_named (GTK_STACK (stack), page, "bt");
  g_signal_connect (qs->bt_switch, "notify::active",
                    G_CALLBACK (on_bt_switch_changed), qs);
  g_signal_connect (qs->bt_scan, "clicked",
                    G_CALLBACK (on_bt_scan_clicked), qs);
  g_signal_connect (qs->bt_list, "row-activated",
                    G_CALLBACK (on_bt_row_activated), qs);
  g_signal_connect (qs->bt_list, "row-selected",
                    G_CALLBACK (on_bt_row_selected), qs);
}

__attribute__((destructor)) static void
bt_auto_cleanup (void)
{
  bt_free_caches ();
}
