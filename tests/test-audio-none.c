/* test-audio-none.c — backend de audio indisponivel (sem pactl/wpctl/amixer).
 *
 * PATH contem SOMENTE um dir vazio: nenhuma ferramenta de audio real pode ser
 * executada. Verifica sliders/combos desabilitados e os caminhos de retorno
 * sem spawn. Precisa de display.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "qs-audio.h"
#include "qs-audio-backend.h"

void audio_refresh_input_volume (QuickSettings *qs);

static gboolean have_display = FALSE;

static QuickSettings *
qs_new (void)
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

/* gtk_widget_get_tooltip_text devolve string nova: liberar apos comparar. */
static void
assert_tooltip (GtkWidget   *widget,
                const gchar *expected)
{
  gchar *tip = gtk_widget_get_tooltip_text (widget);

  g_assert_cmpstr (tip, ==, expected);
  g_free (tip);
}

static void
test_none_probe (void)
{
  g_assert_cmpint (audio_get_backend_cached (), ==, AUDIO_NONE);
  g_assert_false (audio_have_amixer_avail ());
}

static void
test_none_unavailable (void)
{
  QuickSettings *qs;
  GtkWidget     *box;
  GtkWidget     *stack;

  if (skip_no_display ())
    return;
  qs = qs_new ();
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  stack = gtk_stack_new ();
  audio_build_section (box, stack, qs);

  audio_refresh_volume (qs);
  g_assert_false (gtk_widget_is_sensitive (qs->vol_scale));
  assert_tooltip (qs->vol_scale, "Volume unavailable");
  audio_refresh_input_volume (qs);
  g_assert_false (gtk_widget_is_sensitive (qs->audio_in_vol_scale));
  assert_tooltip (qs->audio_in_vol_scale, "Volume unavailable");

  audio_refresh_devices (qs);
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (qs->audio_sub)),
                   ==, "No audio backend");
  g_assert_false (gtk_widget_is_sensitive (qs->sink_combo));
  g_assert_false (gtk_widget_is_sensitive (qs->source_combo));
  gtk_combo_box_set_active (GTK_COMBO_BOX (qs->sink_combo), 0);
  g_assert_cmpstr (gtk_combo_box_get_active_id (GTK_COMBO_BOX (qs->sink_combo)),
                   ==, "none");

  set_volume_percent (50);
  set_source_volume_percent (150);

  qs_unref (qs);
}

int
main (int argc, char **argv)
{
  gchar *bindir;
  gint   ret;

  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check (NULL, NULL);
  bindir = g_dir_make_tmp ("qs-test-audio-none-XXXXXX", NULL);
  g_assert_nonnull (bindir);
  g_setenv ("PATH", bindir, TRUE);

  g_test_add_func ("/audio-none/probe", test_none_probe);
  g_test_add_func ("/audio-none/unavailable", test_none_unavailable);

  ret = g_test_run ();
  g_rmdir (bindir);
  g_free (bindir);
  return ret;
}
