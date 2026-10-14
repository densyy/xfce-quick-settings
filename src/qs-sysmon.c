#include "qs-sysmon.h"
#include "qs-sysmon-data.h"
#include "qs-ui.h"

#define SYSMON_INTERVAL_S 10

/* VIEW (cards/refresh/tick): abaixo só GTK.
 * DATA (acima) nunca toca em widget; VIEW nunca faz parse/stat direto
 * (usa read_* e o mounts_key para pular statvfs). */

/* Resource row: header (title + compact value) over a bar row
 * [expandable bar + % label at right]. The % lives in its own label
 * (the bar never shows overlay text). Value/bar/pct widgets are
 * returned for in-place refresh and also stashed on the row
 * ("qs-sys-value"/"qs-sys-bar"/"qs-sys-pct") so disk rows can be
 * updated without tracking extra state. */
static GtkWidget *
make_sys_card (GtkWidget   *box,
               const gchar *title,
               GtkWidget  **value_out,
               GtkWidget  **bar_out,
               GtkWidget  **pct_out)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_TIGHT_GAP);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *name = gtk_label_new (title);
  GtkWidget *value = gtk_label_new ("—");
  GtkWidget *brow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, QS_INNER_GAP);
  GtkWidget *bar = gtk_progress_bar_new ();
  GtkWidget *pct = gtk_label_new ("—");

  ui_add_class (name, "qs-title");
  gtk_widget_set_halign (name, GTK_ALIGN_START);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  ui_add_class (value, "qs-sys-num");
  gtk_widget_set_halign (value, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (head), name, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (head), value, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (card), head, FALSE, FALSE, 0);
  gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (bar), FALSE);
  gtk_widget_set_hexpand (bar, TRUE);
  gtk_widget_set_halign (bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
  ui_add_class (pct, "qs-sys-num");
  gtk_label_set_width_chars (GTK_LABEL (pct), 4);
  gtk_label_set_xalign (GTK_LABEL (pct), 1.0);
  gtk_widget_set_halign (pct, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (brow), bar, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (brow), pct, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (card), brow, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), card, FALSE, FALSE, 0);
  g_object_set_data (G_OBJECT (card), "qs-sys-value", value);
  g_object_set_data (G_OBJECT (card), "qs-sys-bar", bar);
  g_object_set_data (G_OBJECT (card), "qs-sys-pct", pct);

  if (value_out != NULL)
    *value_out = value;
  if (bar_out != NULL)
    *bar_out = bar;
  if (pct_out != NULL)
    *pct_out = pct;
  return card;
}

static void
sysmon_set_bar (GtkWidget *bar,
                GtkWidget *pct,
                gdouble    fraction)
{
  gchar *text;

  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), fraction);
  text = g_strdup_printf ("%d%%", (gint) (fraction * 100.0 + 0.5));
  gtk_label_set_text (GTK_LABEL (pct), text);
  g_free (text);
}

static void
sysmon_add_separator (GtkWidget *box)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  ui_add_class (sep, "qs-sep");
  gtk_widget_set_margin_top (sep, QS_INNER_GAP);
  gtk_widget_set_margin_bottom (sep, QS_INNER_GAP);
  gtk_box_pack_start (GTK_BOX (box), sep, FALSE, FALSE, 0);
}

static gint
cpu_pair_compare (gconstpointer a,
                  gconstpointer b)
{
  guint64 ua = *(const guint64 *) a;
  guint64 ub = *(const guint64 *) b;

  if (ua < ub)
    return -1;
  if (ua > ub)
    return 1;
  return 0;
}

/* CPU topology ("6C/12T") for the CPU row header value. Physical cores
 * come from distinct (physical id, core id) pairs in /proc/cpuinfo,
 * threads from sysconf. Computed once and cached: topology never
 * changes at runtime. Falls back to "12T" when cores can't be told
 * apart (VMs, ARM without those fields). */
static const gchar *
sysmon_cpu_topology_text (void)
{
  static gchar *cached = NULL;
  glong threads;
  gint cores = 0;

  if (cached != NULL)
    return cached;
  threads = sysconf (_SC_NPROCESSORS_ONLN);
  if (threads <= 0)
    threads = sysconf (_SC_NPROCESSORS_CONF);
  if (threads <= 0)
    threads = 1;
  {
    gchar *content = NULL;

    if (g_file_get_contents ("/proc/cpuinfo", &content, NULL, NULL))
      {
        gchar **lines = g_strsplit (content, "\n", -1);
        GArray *pairs = g_array_new (FALSE, FALSE, sizeof (guint64));
        gint phys = -1;
        gint core = -1;
        gint i;

        g_free (content);
        for (i = 0; lines[i] != NULL; i++)
          {
            if (g_str_has_prefix (lines[i], "physical id"))
              {
                const gchar *colon = strchr (lines[i], ':');

                if (colon != NULL)
                  phys = atoi (colon + 1);
              }
            else if (g_str_has_prefix (lines[i], "core id"))
              {
                const gchar *colon = strchr (lines[i], ':');

                if (colon != NULL)
                  core = atoi (colon + 1);
              }
            else if (lines[i][0] == '\0')
              {
                if (phys >= 0 && core >= 0)
                  {
                    guint64 key = ((guint64) (guint32) phys << 32)
                                  | (guint32) core;

                    g_array_append_val (pairs, key);
                  }
                phys = -1;
                core = -1;
              }
          }
        if (phys >= 0 && core >= 0)
          {
            guint64 key = ((guint64) (guint32) phys << 32) | (guint32) core;

            g_array_append_val (pairs, key);
          }
        if (pairs->len > 0)
          {
            guint j;

            g_array_sort (pairs, cpu_pair_compare);
            cores = 1;
            for (j = 1; j < pairs->len; j++)
              if (g_array_index (pairs, guint64, j)
                  != g_array_index (pairs, guint64, j - 1))
                cores++;
          }
        g_array_free (pairs, TRUE);
        g_strfreev (lines);
      }
  }
  if (cores > 0 && cores <= threads)
    cached = g_strdup_printf ("%dC/%ldT", cores, (glong) threads);
  else
    cached = g_strdup_printf ("%ldT", (glong) threads);
  return cached;
}

static void
sysmon_refresh (QuickSettings *qs)
{
  guint64 total_kb = 0;
  guint64 avail_kb = 0;
  gdouble cpu;
  gchar  *text;

  if (sysmon_read_memory (&total_kb, &avail_kb) && total_kb > 0)
    {
      guint64 used = (total_kb - MIN (avail_kb, total_kb)) * 1024;
      guint64 total = total_kb * 1024;
      gdouble frac = (gdouble) used / (gdouble) total;

      text = g_strdup_printf ("%.2f / %.2f GB",
                              used / 1073741824.0, total / 1073741824.0);
      gtk_label_set_text (GTK_LABEL (qs->sysmon_ram_value), text);
      g_free (text);
      sysmon_set_bar (qs->sysmon_ram_bar, qs->sysmon_ram_pct, frac);
    }
  cpu = sysmon_read_cpu (qs);
  if (cpu >= 0.0)
    {
      /* Header value shows static topology; the side label keeps live %. */
      gtk_label_set_text (GTK_LABEL (qs->sysmon_cpu_value),
                          sysmon_cpu_topology_text ());
      sysmon_set_bar (qs->sysmon_cpu_bar, qs->sysmon_cpu_pct, cpu / 100.0);
    }

  /* Disks: statvfs (bloqueante, pula rede) só quando /proc/mounts mudou
   * (hash mounts_key vs disk_key reaproveitado). Montagens iguais =>
   * mantém barras atuais, sem statvfs na UI. RAM/CPU sempre atualizam
   * (só /proc, barato). Tradeoff: uso de disco congela até (re)montar;
   * evita stalls em FS remoto/stale a cada tick de 10s. */
  {
    gchar *mounts_key = sysmon_mounts_key ();

    if (mounts_key != NULL && qs->sysmon_disk_key != NULL
        && g_strcmp0 (mounts_key, qs->sysmon_disk_key) == 0)
      {
        g_free (mounts_key);
      }
    else
      {
        GPtrArray *disks;
        GString   *key;
        guint      i;

        g_free (mounts_key);
        disks = sysmon_read_disks ();
        key = g_string_new (NULL);

    for (i = 0; i < disks->len; i++)
      {
        SysmonDisk *d = g_ptr_array_index (disks, i);

        g_string_append (key, d->name);
        g_string_append_c (key, '|');
      }
    if (g_strcmp0 (key->str, qs->sysmon_disk_key) == 0)
      {
        GList *cards = gtk_container_get_children (GTK_CONTAINER (qs->sysmon_disk_box));
        GList *l;
        guint  idx = 0;

        for (l = cards; l != NULL && idx < disks->len; l = l->next)
          {
            SysmonDisk *d;
            GtkWidget  *value;
            GtkWidget  *bar;
            GtkWidget  *pct;

            if (g_object_get_data (G_OBJECT (l->data), "qs-sys-bar") == NULL)
              continue;
            d = g_ptr_array_index (disks, idx);
            value = g_object_get_data (G_OBJECT (l->data),
                                       "qs-sys-value");
            bar = g_object_get_data (G_OBJECT (l->data),
                                     "qs-sys-bar");
            pct = g_object_get_data (G_OBJECT (l->data),
                                     "qs-sys-pct");

            if (value != NULL && bar != NULL && pct != NULL
                && GTK_IS_LABEL (value) && GTK_IS_PROGRESS_BAR (bar)
                && GTK_IS_LABEL (pct))
              {
                gtk_label_set_text (GTK_LABEL (value), d->text);
                sysmon_set_bar (bar, pct, d->fraction);
              }
            idx++;
          }
        g_list_free (cards);
        g_string_free (key, TRUE);
      }
    else
      {
        clear_list_box (qs->sysmon_disk_box);
        if (disks->len == 0)
          {
            GtkWidget *empty = gtk_label_new ("No disks found");

            ui_add_class (empty, "qs-status");
            gtk_widget_set_halign (empty, GTK_ALIGN_START);
            gtk_box_pack_start (GTK_BOX (qs->sysmon_disk_box), empty,
                                FALSE, FALSE, 0);
          }
        else
          {
            for (i = 0; i < disks->len; i++)
              {
                SysmonDisk *d = g_ptr_array_index (disks, i);
                GtkWidget  *value = NULL;
                GtkWidget  *bar = NULL;
                GtkWidget  *pct = NULL;

                if (i > 0)
                  sysmon_add_separator (qs->sysmon_disk_box);
                make_sys_card (qs->sysmon_disk_box,
                               d->name, &value, &bar, &pct);
                gtk_label_set_text (GTK_LABEL (value), d->text);
                sysmon_set_bar (bar, pct, d->fraction);
              }
          }
        g_free (qs->sysmon_disk_key);
        qs->sysmon_disk_key = g_string_free (key, FALSE);
        gtk_widget_show_all (qs->sysmon_disk_box);
      }
    sysmon_free_disks (disks);
      }
  }
}

static gboolean
sysmon_tick (gpointer data)
{
  QuickSettings *qs = data;

  if (qs->dead)
    {
      qs->sysmon_timer = 0;
      return G_SOURCE_REMOVE;
    }
  sysmon_refresh (qs);
  return G_SOURCE_CONTINUE;
}

void
sysmon_stop (QuickSettings *qs)
{
  if (qs->sysmon_timer != 0)
    {
      g_source_remove (qs->sysmon_timer);
      qs->sysmon_timer = 0;
    }
}

static void
on_sysmon_tile_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  sysmon_refresh (qs);
  if (qs->sysmon_timer == 0)
    qs->sysmon_timer = g_timeout_add_seconds (SYSMON_INTERVAL_S,
                                              sysmon_tick, qs);
  gtk_stack_set_visible_child_name (GTK_STACK (qs->stack), "sysmon");
}

static void
on_sysmon_back_clicked (GtkButton *button,
                        gpointer   user_data)
{
  QuickSettings *qs = user_data;

  (void) button;
  sysmon_stop (qs);
  show_main_page (qs);
}

static void
on_sysmon_popup_hide (GtkWidget *widget,
                      gpointer   user_data)
{
  (void) widget;
  sysmon_stop (user_data);
}

void
sysmon_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs)
{
  GtkWidget *sys_tile;
  GtkWidget *page;
  GtkWidget *disks_grid;
  GtkWidget *back;

  sys_tile = make_mini_tile (box, "computer-symbolic", "System", NULL);
  g_signal_connect (sys_tile, "clicked",
                    G_CALLBACK (on_sysmon_tile_clicked), qs);

  page = make_sub_page ("System", &back);
  make_sys_card (page, "Memory",
                 &qs->sysmon_ram_value, &qs->sysmon_ram_bar,
                 &qs->sysmon_ram_pct);
  sysmon_add_separator (page);
  make_sys_card (page, "CPU",
                 &qs->sysmon_cpu_value, &qs->sysmon_cpu_bar,
                 &qs->sysmon_cpu_pct);
  sysmon_add_separator (page);
  disks_grid = make_group (page, "Disks");
  qs->sysmon_disk_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, QS_SECTION_GAP);
  gtk_grid_attach (GTK_GRID (disks_grid), qs->sysmon_disk_box, 0, 0, 2, 1);
  gtk_stack_add_named (GTK_STACK (stack), page, "sysmon");
  g_signal_connect (back, "clicked",
                    G_CALLBACK (on_sysmon_back_clicked), qs);
  g_signal_connect (qs->popup, "hide",
                    G_CALLBACK (on_sysmon_popup_hide), qs);
}
