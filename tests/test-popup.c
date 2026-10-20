/* test-popup.c — posicionamento e auto-hide do popup.
 *
 * O host do botão é uma GtkWindow real, mas sem decoração, sem foco e
 * com opacidade 0 (compositor do xfwm4): fica invisível e some no fim
 * de cada caso. O popup, quando só precisa de visibilidade, usa
 * GtkOffscreenWindow. Sem display os casos dão skip.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-popup.h"

static gboolean have_display = FALSE;

static QuickSettings *
new_qs (void)
{
  QuickSettings *qs = g_new0 (QuickSettings, 1);

  qs_ref (qs);
  return qs;
}

static gboolean
skip_no_display (void)
{
  if (!have_display)
    {
      g_test_skip ("needs display");
      return TRUE;
    }
  return FALSE;
}

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit ((GMainLoop *) data);
  return G_SOURCE_REMOVE;
}

static void
run_loop_ms (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static GdkRectangle
primary_workarea (void)
{
  GdkMonitor   *monitor = gdk_display_get_monitor (gdk_display_get_default (), 0);
  GdkRectangle  area;

  g_assert_nonnull (monitor);
  gdk_monitor_get_workarea (monitor, &area);
  return area;
}

/* Host invisível: botão em (0,0) dentro de um GtkFixed. Espera a janela
 * mapear na posição pedida pelo WM antes de devolver. */
static GtkWidget *
make_host (GtkWidget  *button,
           GtkWidget **fixed_out,
           gint        screen_x,
           gint        screen_y,
           gint        width,
           gint        height)
{
  GtkWidget *host = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  GtkWidget *fixed = gtk_fixed_new ();
  gint       waited = 0;

  gtk_window_set_decorated (GTK_WINDOW (host), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (host), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (host), TRUE);
  gtk_window_set_accept_focus (GTK_WINDOW (host), FALSE);
  gtk_window_set_focus_on_map (GTK_WINDOW (host), FALSE);
  gtk_widget_set_opacity (host, 0.0);
  gtk_widget_set_size_request (host, width, height);
  gtk_container_add (GTK_CONTAINER (host), fixed);
  gtk_fixed_put (GTK_FIXED (fixed), button, 0, 0);
  gtk_window_move (GTK_WINDOW (host), screen_x, screen_y);
  gtk_widget_realize (host);
  if (gtk_widget_get_window (host) != NULL)
    gdk_window_set_override_redirect (gtk_widget_get_window (host), TRUE);
  gtk_widget_show_all (host);

  while (waited < 500)
    {
      gint ox = -1;
      gint oy = -1;

      run_loop_ms (20);
      waited += 20;
      if (gtk_widget_get_window (button) == NULL)
        continue;
      gdk_window_get_origin (gtk_widget_get_window (button), &ox, &oy);
      if (ox == screen_x && oy == screen_y)
        break;
    }
  *fixed_out = fixed;
  return host;
}

/* Posição com tamanhos inválidos usa 320x200 e clampa na esquerda. */
static void
test_position_defaults (void)
{
  QuickSettings *qs;
  GtkWidget     *button;
  GtkWidget     *fixed;
  GtkWidget     *host;
  GtkWidget     *popup;
  GdkRectangle   area;
  GtkAllocation  allocation;
  GdkWindow     *button_window;
  gint           origin_x = 0;
  gint           origin_y = 0;
  gint           x = 0;
  gint           y = 0;

  if (skip_no_display ())
    return;

  qs = new_qs ();
  button = gtk_button_new_with_label ("qs");
  popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->button = button;
  qs->popup = popup;
  area = primary_workarea ();

  /* Sem GdkWindow o posicionamento é um no-op seguro. */
  qs_popup_position_at_size (qs, 320, 200);

  host = make_host (button, &fixed, area.x + 2, area.y + 100, 60, 60);
  (void) fixed;
  gtk_widget_get_allocation (button, &allocation);
  button_window = gtk_widget_get_window (button);
  g_assert_nonnull (button_window);
  gdk_window_get_origin (button_window, &origin_x, &origin_y);

  g_assert_cmpint (origin_x + allocation.x + allocation.width - 320,
                   <, area.x + 6);
  qs_popup_position_at_size (qs, 0, 0);
  gtk_window_get_position (GTK_WINDOW (popup), &x, &y);
  g_assert_cmpint (x, ==, area.x + 6);
  g_assert_cmpint (y, ==, origin_y + allocation.y + allocation.height + 6);

  gtk_widget_destroy (host);
  gtk_widget_destroy (popup);
  qs_unref (qs);
}

/* Bordas direita e inferior: reposiciona o host e usa tamanho grande. */
static void
test_position_clamps (void)
{
  QuickSettings *qs;
  GtkWidget     *button;
  GtkWidget     *fixed;
  GtkWidget     *host;
  GtkWidget     *popup;
  GdkRectangle   area;
  GtkAllocation  allocation;
  GdkWindow     *button_window;
  gint           origin_x = 0;
  gint           origin_y = 0;
  gint           x = 0;
  gint           y = 0;

  if (skip_no_display ())
    return;

  qs = new_qs ();
  button = gtk_button_new_with_label ("qs");
  popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  qs->button = button;
  qs->popup = popup;
  area = primary_workarea ();

  host = make_host (button, &fixed, area.x + area.width - 80,
                    area.y + area.height - 100, 80, 100);
  /* Botão no canto direito/inferior do host para as bordas dispararem. */
  gtk_fixed_move (GTK_FIXED (fixed), button, 40, 72);
  run_loop_ms (100);
  gtk_widget_get_allocation (button, &allocation);
  button_window = gtk_widget_get_window (button);
  g_assert_nonnull (button_window);
  gdk_window_get_origin (button_window, &origin_x, &origin_y);

  g_assert_cmpint (origin_x + allocation.x + allocation.width,
                   >, area.x + area.width - 6);
  qs_popup_position_at_size (qs, 800, 300);
  gtk_window_get_position (GTK_WINDOW (popup), &x, &y);
  g_assert_cmpint (x, ==, area.x + area.width - 800 - 6);

  g_assert_cmpint (origin_y + allocation.y + allocation.height,
                   >, area.y + area.height - 6);
  qs_popup_position_at_size (qs, 800, area.height);
  gtk_window_get_position (GTK_WINDOW (popup), &x, &y);
  g_assert_cmpint (y, ==, area.y + 6);

  /* Caminho legado: mede via gtk_window_get_size e delega. */
  gtk_window_set_default_size (GTK_WINDOW (popup), 640, 480);
  qs_popup_position (qs);
  gtk_window_get_position (GTK_WINDOW (popup), &x, &y);
  g_assert_cmpint (x, >=, area.x + 6);
  g_assert_cmpint (y, >=, area.y + 6);

  gtk_widget_destroy (host);
  gtk_widget_destroy (popup);
  qs_unref (qs);
}

/* Popup real, invisível (opacidade 0) e sem foco: grab e visibilidade
 * funcionam como no plugin sem interferir no desktop. */
static GtkWidget *
make_popup (void)
{
  GtkWidget *popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);

  gtk_window_set_decorated (GTK_WINDOW (popup), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (popup), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (popup), TRUE);
  gtk_window_set_accept_focus (GTK_WINDOW (popup), FALSE);
  gtk_window_set_focus_on_map (GTK_WINDOW (popup), FALSE);
  gtk_widget_set_opacity (popup, 0.0);
  return popup;
}

/* Move o popup (ainda oculto) para longe do ponteiro: mapear embaixo
 * dele troca a janela do device e invalida as coordenadas relativas. */
static void
park_popup (GtkWidget   *popup,
            gint         ptr_x,
            gint         ptr_y,
            GdkRectangle area)
{
  gint x = (ptr_x < area.x + area.width / 2)
           ? area.x + area.width - 220 : area.x;
  gint y = (ptr_y < area.y + area.height / 2)
           ? area.y + area.height - 220 : area.y + 40;

  gtk_window_move (GTK_WINDOW (popup), MAX (x, area.x), MAX (y, area.y));
}

/* Coordenadas do ponteiro relativas à janela do botão. */
static void
pointer_rel (GtkWidget *button,
             gint      *rel_x,
             gint      *rel_y)
{
  GdkSeat       *seat = gdk_display_get_default_seat (gdk_display_get_default ());
  GdkDevice     *pointer = gdk_seat_get_pointer (seat);
  GdkWindow     *window = gtk_widget_get_window (button);

  g_assert_nonnull (pointer);
  g_assert_nonnull (window);
  gdk_window_get_device_position (window, pointer, rel_x, rel_y, NULL);
}

/* Ponteiro: sem botão, sem janela, sobre e fora da alocação do botão. */
static void
test_pointer_on_button (void)
{
  QuickSettings *qs;
  GtkWidget     *button;
  GtkWidget     *fixed;
  GtkWidget     *host;
  GdkRectangle   area;
  GdkSeat       *seat;
  GdkDevice     *pointer;
  GtkAllocation  allocation;
  gint           ptr_x = 0;
  gint           ptr_y = 0;
  gint           rel_x = 0;
  gint           rel_y = 0;

  if (skip_no_display ())
    return;

  qs = new_qs ();
  g_assert_false (qs_popup_pointer_on_button (qs));

  button = gtk_button_new_with_label ("qs");
  qs->button = button;
  g_assert_false (qs_popup_pointer_on_button (qs));

  area = primary_workarea ();
  seat = gdk_display_get_default_seat (gdk_display_get_default ());
  g_assert_nonnull (seat);
  pointer = gdk_seat_get_pointer (seat);
  g_assert_nonnull (pointer);
  gdk_device_get_position (pointer, NULL, &ptr_x, &ptr_y);

  /* Host mapeado longe do ponteiro: FALSE. */
  host = make_host (button, &fixed,
                    (ptr_x < area.x + area.width / 2)
                    ? area.x + area.width - 60 : area.x,
                    (ptr_y < area.y + area.height / 2)
                    ? area.y + area.height - 40 : area.y,
                    60, 40);
  pointer_rel (button, &rel_x, &rel_y);
  gtk_widget_get_allocation (button, &allocation);
  g_assert_true (rel_x < 0 || rel_y < 0
                 || rel_x >= allocation.width || rel_y >= allocation.height);
  g_assert_false (qs_popup_pointer_on_button (qs));
  gtk_widget_destroy (host);

  /* Host mapeado debaixo do ponteiro, botão em (0,0): TRUE. */
  button = gtk_button_new_with_label ("qs");
  qs->button = button;
  host = make_host (button, &fixed, ptr_x - 20, ptr_y - 20, 60, 40);
  pointer_rel (button, &rel_x, &rel_y);
  gtk_widget_get_allocation (button, &allocation);
  if (rel_x < 0 || rel_y < 0
      || rel_x >= allocation.width || rel_y >= allocation.height)
    {
      g_test_skip ("pointer on workarea edge");
      gtk_widget_destroy (host);
      qs_unref (qs);
      return;
    }
  g_assert_true (qs_popup_pointer_on_button (qs));

  gtk_widget_destroy (host);
  qs_unref (qs);
}

/* Focus-out: invisível, grab ativo, ponteiro no botão e clique fora. */
static void
test_focus_out (void)
{
  QuickSettings *qs;
  GtkWidget     *button;
  GtkWidget     *fixed;
  GtkWidget     *host;
  GtkWidget     *popup;
  GdkSeat       *seat;
  GdkDevice     *pointer;
  GdkRectangle   area;
  GtkAllocation  allocation;
  gboolean       pointer_ok;
  gint           ptr_x = 0;
  gint           ptr_y = 0;
  gint           rel_x = 0;
  gint           rel_y = 0;

  if (skip_no_display ())
    return;

  qs = new_qs ();
  button = gtk_button_new_with_label ("qs");
  popup = make_popup ();
  qs->button = button;
  qs->popup = popup;
  area = primary_workarea ();
  seat = gdk_display_get_default_seat (gdk_display_get_default ());
  pointer = gdk_seat_get_pointer (seat);
  g_assert_nonnull (pointer);
  gdk_device_get_position (pointer, NULL, &ptr_x, &ptr_y);

  /* Botão longe do ponteiro: clique fora fecha. */
  host = make_host (button, &fixed,
                    (ptr_x < area.x + area.width / 2)
                    ? area.x + area.width - 60 : area.x,
                    (ptr_y < area.y + area.height / 2)
                    ? area.y + area.height - 40 : area.y,
                    60, 40);
  pointer_rel (button, &rel_x, &rel_y);
  gtk_widget_get_allocation (button, &allocation);
  g_assert_true (rel_x < 0 || rel_y < 0
                 || rel_x >= allocation.width || rel_y >= allocation.height);

  g_assert_false (qs_popup_on_focus_out (popup, NULL, qs));
  g_assert_false (gtk_widget_get_visible (popup));

  park_popup (popup, ptr_x, ptr_y, area);
  gtk_widget_show (popup);
  run_loop_ms (50);
  g_assert_true (gtk_widget_get_visible (popup));
  g_assert_false (qs_popup_on_focus_out (popup, NULL, qs));
  g_assert_false (gtk_widget_get_visible (popup));
  gtk_widget_destroy (host);

  /* Host mapeado debaixo do ponteiro: o botão não fecha o popup. */
  button = gtk_button_new_with_label ("qs");
  qs->button = button;
  host = make_host (button, &fixed, ptr_x - 20, ptr_y - 20, 60, 40);
  pointer_rel (button, &rel_x, &rel_y);
  gtk_widget_get_allocation (button, &allocation);
  pointer_ok = (rel_x >= 0 && rel_y >= 0
                && rel_x < allocation.width && rel_y < allocation.height);
  if (pointer_ok)
    {
      g_assert_true (qs_popup_pointer_on_button (qs));
      park_popup (popup, ptr_x, ptr_y, area);
      gtk_widget_show (popup);
      run_loop_ms (50);
      g_assert_false (qs_popup_on_focus_out (popup, NULL, qs));
      g_assert_true (gtk_widget_get_visible (popup));
    }

  /* Grab em processo: não fecha. */
  park_popup (popup, ptr_x, ptr_y, area);
  gtk_widget_show (popup);
  run_loop_ms (50);
  gtk_grab_add (popup);
  g_assert_false (qs_popup_on_focus_out (popup, NULL, qs));
  g_assert_true (gtk_widget_get_visible (popup));
  gtk_grab_remove (popup);

  gtk_widget_destroy (host);
  gtk_widget_destroy (popup);
  qs_unref (qs);

  if (!pointer_ok)
    g_test_skip ("pointer on workarea edge");
}

/* Escape fecha; outra tecla não. */
static void
test_key_press (void)
{
  QuickSettings *qs;
  GtkWidget     *popup;
  GdkEvent      *event;

  if (skip_no_display ())
    return;

  qs = new_qs ();
  popup = make_popup ();
  qs->popup = popup;
  gtk_widget_show (popup);
  run_loop_ms (50);

  event = gdk_event_new (GDK_KEY_PRESS);
  ((GdkEventKey *) event)->keyval = GDK_KEY_a;
  g_assert_false (qs_popup_on_key_press (popup, (GdkEventKey *) event, qs));
  g_assert_true (gtk_widget_get_visible (popup));
  gdk_event_free (event);

  event = gdk_event_new (GDK_KEY_PRESS);
  ((GdkEventKey *) event)->keyval = GDK_KEY_Escape;
  g_assert_true (qs_popup_on_key_press (popup, (GdkEventKey *) event, qs));
  g_assert_false (gtk_widget_get_visible (popup));
  gdk_event_free (event);

  event = gdk_event_new (GDK_KEY_PRESS);
  ((GdkEventKey *) event)->keyval = GDK_KEY_Escape;
  g_assert_false (qs_popup_on_key_press (popup, (GdkEventKey *) event, qs));
  gdk_event_free (event);

  gtk_widget_destroy (popup);
  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *tmp_root;
  gint   ret;

  g_test_init (&argc, &argv, NULL);
  tmp_root = g_dir_make_tmp ("qs-test-popup-XXXXXX", NULL);
  g_assert_nonnull (tmp_root);
  g_setenv ("XDG_CACHE_HOME", tmp_root, TRUE);
  g_setenv ("XDG_DATA_HOME", tmp_root, TRUE);
  have_display = gtk_init_check (NULL, NULL);

  g_test_add_func ("/popup/position_defaults", test_position_defaults);
  g_test_add_func ("/popup/position_clamps", test_position_clamps);
  g_test_add_func ("/popup/pointer_on_button", test_pointer_on_button);
  g_test_add_func ("/popup/focus_out", test_focus_out);
  g_test_add_func ("/popup/key_press", test_key_press);

  ret = g_test_run ();
  g_free (tmp_root);
  return ret;
}
