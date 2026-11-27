#include "qs-display.h"
#include "qs-monitor-data.h"
#include "qs-ui.h"
#include "qs-util.h"

#include <string.h>

static void
display_update_power_switch (QuickSettings *qs,
                             QsMonInfo     *sel_mon,
                             gint           n_active)
{
  gboolean on = FALSE;
  gboolean only = FALSE;
  const gchar *cur_status;

  if (qs->mon_switch == NULL)
    return;
  if (sel_mon != NULL)
    {
      on = sel_mon->active;
      only = (on && n_active <= 1);
    }
  gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), on);
  gtk_widget_set_sensitive (qs->mon_switch,
                            (sel_mon != NULL && !only));
  if (sel_mon == NULL)
    {
      gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
      return;
    }
  if (only)
    {
      gtk_widget_set_tooltip_text (qs->mon_switch,
                                   "Cannot disable the only display");
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                          "Cannot disable the only display.");
    }
  else
    {
      gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
      /* Drop a stale guard message when the selection now allows off. */
      cur_status = gtk_label_get_text (GTK_LABEL (qs->display_status));
      if (g_strcmp0 (cur_status, "Cannot disable the only display.") == 0)
        gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
    }
}

GtkWidget *
make_combo_row (GtkWidget   *grid,
                gint         row,
                const gchar *icon_name,
                const gchar *tooltip)
{
  GtkWidget *icon;
  GtkWidget *combo;

  icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), QS_ROW_ICON_PX);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_grid_attach (GTK_GRID (grid), icon, 0, row, 1, 1);

  combo = gtk_combo_box_text_new ();
  ui_add_class (combo, "qs-combo");
  gtk_widget_set_hexpand (combo, TRUE);
  gtk_widget_set_tooltip_text (combo, tooltip);
  gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);

  return combo;
}

static void
fill_combo (GtkWidget   *combo,
            GList       *items,
            const gchar *active,
            const gchar *empty_text)
{
  GList *l;

  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
  for (l = items; l != NULL; l = l->next)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                               (const gchar *) l->data,
                               (const gchar *) l->data);
  if (items == NULL)
    {
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                 "none", empty_text);
      gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
      gtk_widget_set_sensitive (combo, FALSE);
      return;
    }
  gtk_widget_set_sensitive (combo, TRUE);
  if (active != NULL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), active);
  if (gtk_combo_box_get_active (GTK_COMBO_BOX (combo)) < 0)
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
}

static void
display_update_sub (QuickSettings *qs,
                    const gchar   *mon,
                    const gchar   *mode)
{
  gchar *text;

  if (mon == NULL)
    text = g_strdup ("No displays");
  else if (mode != NULL)
    text = g_strdup_printf ("%s · %s", mon, mode);
  else
    text = g_strdup (mon);
  gtk_label_set_text (GTK_LABEL (qs->display_sub), text);
  g_free (text);
}

static void
mon_switch_off (QuickSettings *qs)
{
  if (qs->mon_switch == NULL)
    return;
  gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), FALSE);
  gtk_widget_set_sensitive (qs->mon_switch, FALSE);
  gtk_widget_set_tooltip_text (qs->mon_switch, "Monitor on/off");
}

static void
display_unavailable (QuickSettings *qs)
{
  qs->updating = TRUE;
  fill_combo (qs->mon_combo, NULL, NULL, "unavailable");
  fill_combo (qs->mode_combo, NULL, NULL, "unavailable");
  fill_combo (qs->scale_combo, NULL, NULL, "unavailable");
  mon_switch_off (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                       "xrandr not found (unavailable).");
  gtk_label_set_text (GTK_LABEL (qs->display_sub), "unavailable");
  gtk_widget_set_sensitive (qs->display_tile, FALSE);
  qs->updating = FALSE;
}

static void
fill_scale_combo (QuickSettings *qs)
{
  gsize i;

  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (qs->scale_combo));
  for (i = 0; i < G_N_ELEMENTS (SCALE_FACTORS); i++)
    {
      gchar *label = g_strdup_printf ("%sx", SCALE_FACTORS[i]);

      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (qs->scale_combo),
                                 SCALE_FACTORS[i], label);
      g_free (label);
    }
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (qs->scale_combo), "1");
  gtk_widget_set_sensitive (qs->scale_combo, TRUE);
}

static void
query_fail (QuickSettings *qs)
{
  fill_combo (qs->mon_combo, NULL, NULL, "No displays");
  fill_combo (qs->mode_combo, NULL, NULL, "No displays");
  fill_combo (qs->scale_combo, NULL, NULL, "No displays");
  mon_switch_off (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                       "Could not query displays.");
  display_update_sub (qs, NULL, NULL);
  qs->updating = FALSE;
  qs_unref (qs);
}

static void
query_fill_selection (QuickSettings *qs,
                      QsDisplaySnap *snap,
                      const gchar   *sel)
{
  QsMonInfo *sel_mon = qs_snap_find (snap, sel);
  const gchar *shown_mode = NULL;

  if (sel_mon != NULL)
    {
      shown_mode = (sel_mon->current != NULL) ? sel_mon->current
                   : (sel_mon->modes != NULL
                      ? (const gchar *) sel_mon->modes->data : NULL);
      fill_combo (qs->mode_combo, sel_mon->modes, shown_mode, "No modes");
    }
  else
    fill_combo (qs->mode_combo, NULL, NULL, "No modes");
  fill_scale_combo (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
  if (sel_mon != NULL && sel_mon->active)
    display_update_sub (qs, sel, shown_mode);
  else
    display_update_sub (qs, sel, "Off");
  display_update_power_switch (qs, sel_mon, snap->n_active);
}

static void
query_paint_no_selection (QuickSettings *qs)
{
  fill_combo (qs->mode_combo, NULL, NULL, "No displays");
  fill_combo (qs->scale_combo, NULL, NULL, "No displays");
  mon_switch_off (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                       "No connected displays.");
  display_update_sub (qs, NULL, NULL);
}

static void
display_query_done (const gchar *output,
                    const gchar *errout,
                    gboolean     ok,
                    gpointer     user_data)
{
  QuickSettings *qs = user_data;
  gchar         *prev;
  gchar         *sel = NULL;
  QsDisplaySnap *snap = NULL;
  GList         *names = NULL;
  GList         *l;

  (void) errout;
  if (g_atomic_int_get (&qs->dead))
    {
      qs_unref (qs);
      return;
    }
  qs->display_loading = FALSE;
  qs->updating = TRUE;

  if (!ok || output == NULL || *output == '\0')
    {
      query_fail (qs);
      return;
    }

  g_free (qs->display_last);
  qs->display_last = g_strdup (output);

  snap = qs_display_snap_new (output);

  prev = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo)));
  if (prev != NULL
      && (g_strcmp0 (prev, "none") == 0 || !valid_monitor_shared (prev)))
    {
      g_free (prev);
      prev = NULL;
    }
  for (l = snap->mons; l != NULL; l = l->next)
    {
      QsMonInfo *m = l->data;

      names = g_list_append (names, m->name);
      if (prev != NULL && g_strcmp0 (m->name, prev) == 0)
        sel = g_strdup (m->name);
    }
  g_free (prev);
  if (sel == NULL && snap->mons != NULL)
    sel = g_strdup (((QsMonInfo *) snap->mons->data)->name);
  fill_combo (qs->mon_combo, names, sel, "No displays");
  g_list_free (names);

  if (sel != NULL)
    query_fill_selection (qs, snap, sel);
  else
    query_paint_no_selection (qs);

  g_free (sel);
  qs_display_snap_free (snap);
  qs->updating = FALSE;
  qs_unref (qs);
}

void
display_load_async (QuickSettings *qs)
{
  if (!command_exists ("xrandr"))
    {
      display_unavailable (qs);
      return;
    }
  if (qs->display_loading)
    return;
  qs->display_loading = TRUE;
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status), "Loading…");
  run_command_async ("xrandr --query", display_query_done, qs);
}

void
on_mon_changed (GtkComboBox *combo,
                gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *id;
  QsDisplaySnap *snap = NULL;
  QsMonInfo     *sel_mon;
  const gchar   *shown;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  id = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor_shared (id) || qs->display_last == NULL)
    return;
  /* Um parse do cache; antes eram 3 (modes + active + counts). */
  snap = qs_display_snap_new (qs->display_last);
  sel_mon = qs_snap_find (snap, id);
  if (sel_mon == NULL)
    {
      qs_display_snap_free (snap);
      return;
    }
  qs->updating = TRUE;
  shown = (sel_mon->current != NULL) ? sel_mon->current
          : (sel_mon->modes != NULL
             ? (const gchar *) sel_mon->modes->data : NULL);
  fill_combo (qs->mode_combo, sel_mon->modes, shown, "No modes");
  fill_scale_combo (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status), NULL);
  if (sel_mon->active)
    display_update_sub (qs, id, shown);
  else
    display_update_sub (qs, id, "Off");
  display_update_power_switch (qs, sel_mon, snap->n_active);
  qs_display_snap_free (snap);
  qs->updating = FALSE;
}

void
on_mode_changed (GtkComboBox *combo,
                 gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *mon;
  const gchar   *mode;
  gchar         *qmon;
  gchar         *qmode;
  gchar         *cmd;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  mode = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor_shared (mon) || !valid_mode (mode))
    return;
  qmon = g_shell_quote (mon);
  qmode = g_shell_quote (mode);
  cmd = g_strdup_printf ("xrandr --output %s --mode %s", qmon, qmode);
  g_free (qmon);
  g_free (qmode);
  spawn_command (cmd);
  g_free (cmd);
  display_update_sub (qs, mon, mode);
}

static const gchar *
scale_inverse (const gchar *factor)
{
  if (g_strcmp0 (factor, "1.25") == 0)
    return "0.8";
  if (g_strcmp0 (factor, "1.5") == 0)
    return "0.667";
  if (g_strcmp0 (factor, "1.75") == 0)
    return "0.571";
  if (g_strcmp0 (factor, "2") == 0)
    return "0.5";
  return "1";
}

void
on_scale_changed (GtkComboBox *combo,
                  gpointer     user_data)
{
  QuickSettings *qs = user_data;
  const gchar   *mon;
  const gchar   *factor;
  const gchar   *inv;
  gchar         *qmon;
  gchar         *cmd;

  if (qs->updating)
    return;
  if (!gtk_widget_is_sensitive (GTK_WIDGET (combo)))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  factor = gtk_combo_box_get_active_id (combo);
  if (!valid_monitor_shared (mon) || !valid_scale (factor))
    return;
  inv = scale_inverse (factor);
  qmon = g_shell_quote (mon);
  cmd = g_strdup_printf ("xrandr --output %s --scale %sx%s",
                         qmon, inv, inv);
  g_free (qmon);
  spawn_command (cmd);
  g_free (cmd);
}

typedef struct
{
  QuickSettings *qs;
  gchar         *mon;
  gboolean       turn_on;
} MonPower;

static gchar *
mon_error_detail (const gchar *errout)
{
  gchar *detail;

  if (errout == NULL || *errout == '\0')
    return NULL;
  detail = g_strdup (errout);
  detail[strcspn (detail, "\n")] = '\0';
  g_strstrip (detail);
  if (*detail == '\0')
    {
      g_free (detail);
      return NULL;
    }
  return detail;
}

static void
mon_power_fail (QuickSettings *qs,
                MonPower      *c,
                gchar         *detail)
{
  qs->updating = TRUE;
  if (qs->mon_switch != NULL)
    gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), !c->turn_on);
  qs->updating = FALSE;
  if (detail != NULL)
    {
      gchar *msg = g_strdup_printf ("Failed to turn monitor %s: %s",
                                    c->turn_on ? "on" : "off", detail);

      gtk_label_set_text (GTK_LABEL (qs->display_status), msg);
      g_free (msg);
      g_free (detail);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                          c->turn_on ? "Failed to turn monitor on."
                                     : "Failed to turn monitor off.");
    }
}

static void
mon_power_done (const gchar *output,
                const gchar *errout,
                gboolean     ok,
                gpointer     user_data)
{
  MonPower *c = user_data;
  QuickSettings *qs = c->qs;

  (void) output;
  if (g_atomic_int_get (&qs->dead))
    {
      g_free (c->mon);
      qs_unref (qs);
      g_free (c);
      return;
    }
  if (ok)
    {
      gtk_label_set_text (GTK_LABEL (qs->display_status),
                          c->turn_on ? "Turning monitor on…"
                                     : "Turning monitor off…");
      display_load_async (qs);
    }
  else
    mon_power_fail (qs, c, mon_error_detail (errout));
  g_free (c->mon);
  qs_unref (qs);
  g_free (c);
}

static void
mon_set_power_async (QuickSettings *qs,
                     const gchar   *mon,
                     gboolean       turn_on)
{
  MonPower *c;
  gchar    *qmon;
  gchar    *cmd;

  qmon = g_shell_quote (mon);
  if (turn_on)
    cmd = g_strdup_printf ("xrandr --output %s --auto", qmon);
  else
    cmd = g_strdup_printf ("xrandr --output %s --off", qmon);
  g_free (qmon);
  c = g_new0 (MonPower, 1);
  c->qs = qs;
  c->mon = g_strdup (mon);
  c->turn_on = turn_on;
  qs_ref (qs);
  gtk_label_set_text (GTK_LABEL (qs->display_status),
                      turn_on ? "Turning monitor on…"
                              : "Turning monitor off…");
  run_command_async (cmd, mon_power_done, c);
  g_free (cmd);
}

/* Same switch pattern as Wi-Fi/BT; re-checks the single-display guard. */
void
on_mon_power_changed (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  QuickSettings *qs = user_data;
  gboolean       on = gtk_switch_get_active (GTK_SWITCH (obj));
  const gchar   *mon;
  QsDisplaySnap *snap = NULL;
  QsMonInfo     *sel_mon;

  (void) pspec;
  if (qs->updating)
    return;
  if (qs->mon_switch == NULL
      || !gtk_widget_is_sensitive (qs->mon_switch))
    return;
  mon = gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->mon_combo));
  if (!valid_monitor_shared (mon))
    return;
  if (!on && qs->display_last != NULL)
    {
      /* Um parse do cache: cobre count + active. */
      snap = qs_display_snap_new (qs->display_last);
      sel_mon = qs_snap_find (snap, mon);
      if (sel_mon != NULL && sel_mon->active && snap->n_active <= 1)
        {
          qs->updating = TRUE;
          gtk_switch_set_active (GTK_SWITCH (qs->mon_switch), TRUE);
          gtk_widget_set_sensitive (qs->mon_switch, FALSE);
          gtk_widget_set_tooltip_text (qs->mon_switch,
                                       "Cannot disable the only display");
          qs->updating = FALSE;
          gtk_label_set_text (GTK_LABEL (qs->display_status),
                              "Cannot disable the only display.");
          qs_display_snap_free (snap);
          return;
        }
      qs_display_snap_free (snap);
    }
  mon_set_power_async (qs, mon, on);
}

void
display_refresh_tile (QuickSettings *qs)
{
  night_update_sub (qs);
  if (!command_exists ("xrandr"))
    {
      display_unavailable (qs);
      return;
    }
  gtk_widget_set_sensitive (qs->display_tile, TRUE);
  display_load_async (qs);
}
