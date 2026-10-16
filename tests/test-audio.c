/* test-audio.c — GTest para qs-audio-backend (grupo 8).
 * Usa tests/fakebin/pactl via PATH (só fakebin: sem amixer). */
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "qs-audio-backend.h"
#include "qs-audio.h"

static const gchar *MON_SOURCES =
  "Source #0\n"
  "\tName: alsa_input.pci-0\n"
  "\tMonitor of Sink: n/a\n"
  "Source #1\n"
  "\tName: alsa_output.pci-0.monitor\n"
  "\tMonitor of Sink: 0\n";

static const gchar *DESCS =
  "Sink #0\n"
  "\tName: alsa_output.pci-0\n"
  "\tDescription: Built-in Audio\n"
  "Sink #1\n"
  "\tName: bluez_output.AB\n"
  "\tDescription: Headphones\n";

static void
test_backend_probe (void)
{
  /* PATH só tem fakebin: pactl presente, wpctl/amixer ausentes. */
  g_assert_cmpint (audio_get_backend_cached (), ==, AUDIO_PACTL);
  g_assert_false (audio_have_amixer_avail ());
}

static void
test_volume_icon (void)
{
  g_assert_cmpstr (volume_icon_for_level (101), ==, "audio-volume-overamplified-symbolic");
  g_assert_cmpstr (volume_icon_for_level (100), ==, "audio-volume-high-symbolic");
  g_assert_cmpstr (volume_icon_for_level (67), ==, "audio-volume-high-symbolic");
  g_assert_cmpstr (volume_icon_for_level (66), ==, "audio-volume-medium-symbolic");
  g_assert_cmpstr (volume_icon_for_level (34), ==, "audio-volume-medium-symbolic");
  g_assert_cmpstr (volume_icon_for_level (33), ==, "audio-volume-low-symbolic");
  g_assert_cmpstr (volume_icon_for_level (1), ==, "audio-volume-low-symbolic");
  g_assert_cmpstr (volume_icon_for_level (0), ==, "audio-volume-muted-symbolic");
  g_assert_cmpstr (volume_icon_for_level (-5), ==, "audio-volume-muted-symbolic");
}

static void
test_dev_new (void)
{
  AudioDev *d1 = audio_dev_new ("id-1", NULL, FALSE);
  AudioDev *d2 = audio_dev_new ("id-2", "Label", TRUE);

  g_assert_cmpstr (d1->id, ==, "id-1");
  g_assert_cmpstr (d1->label, ==, "id-1");
  g_assert_false (d1->is_default);
  g_assert_cmpstr (d2->label, ==, "Label");
  g_assert_true (d2->is_default);
  audio_dev_free (d1);
  audio_dev_free (d2);
}

static void
test_monitor_sources (void)
{
  GHashTable *map = parse_pactl_monitor_sources (MON_SOURCES);

  g_assert_nonnull (map);
  /* Monitor real entra; "Monitor of Sink: n/a" é pulado. */
  g_assert_nonnull (g_hash_table_lookup (map, "alsa_output.pci-0.monitor"));
  g_assert_null (g_hash_table_lookup (map, "alsa_input.pci-0"));
  g_hash_table_destroy (map);
}

static void
test_descriptions (void)
{
  GHashTable *map = parse_pactl_descriptions (DESCS, "Sink #");

  g_assert_nonnull (map);
  g_assert_cmpstr (g_hash_table_lookup (map, "alsa_output.pci-0"), ==, "Built-in Audio");
  g_assert_cmpstr (g_hash_table_lookup (map, "bluez_output.AB"), ==, "Headphones");
  g_assert_null (g_hash_table_lookup (map, "nope"));
  g_hash_table_destroy (map);
}

static void
test_get_default (void)
{
  gchar *d = pactl_get_default ("pactl get-default-sink");

  g_assert_cmpstr (d, ==, "alsa_output.pci-0");
  g_free (d);
}

static AudioDev *
find_dev (GList *devs, const gchar *id)
{
  GList *l;

  for (l = devs; l != NULL; l = l->next)
    {
      AudioDev *d = l->data;

      if (g_strcmp0 (d->id, id) == 0)
        return d;
    }
  return NULL;
}

static void
test_list_sinks (void)
{
  GList    *sinks = list_pactl_sinks ();
  AudioDev *d;

  g_assert_nonnull (sinks);
  g_assert_cmpuint (g_list_length (sinks), ==, 2);
  d = find_dev (sinks, "alsa_output.pci-0");
  g_assert_nonnull (d);
  g_assert_cmpstr (d->label, ==, "Built-in Audio");
  g_assert_true (d->is_default);
  d = find_dev (sinks, "bluez_output.AB");
  g_assert_nonnull (d);
  g_assert_false (d->is_default);
  g_list_free_full (sinks, audio_dev_free);
}

static void
test_list_sources (void)
{
  GList    *srcs = list_pactl_sources ();
  AudioDev *d;

  /* .monitor e monitores de sink são excluídos: só a fonte real. */
  g_assert_nonnull (srcs);
  g_assert_cmpuint (g_list_length (srcs), ==, 1);
  d = srcs->data;
  g_assert_cmpstr (d->id, ==, "alsa_input.pci-0");
  g_assert_cmpstr (d->label, ==, "Built-in Mic");
  g_assert_true (d->is_default);
  g_list_free_full (srcs, audio_dev_free);
}

static void
test_move_streams (void)
{
  const gchar *logf = "/tmp/qs-test-audio-move.log";
  gchar       *log = NULL;
  gint         i;
  gboolean     got5 = FALSE;
  gboolean     got9 = FALSE;

  g_remove (logf);
  g_setenv ("QS_FAKE_LOG", logf, TRUE);
  pactl_move_streams ("pactl list short sink-inputs",
                      "pactl move-sink-input", "alsa_output.pci-0");
  /* spawn é async: espera o fake logar (até ~2s). */
  for (i = 0; i < 20 && !(got5 && got9); i++)
    {
      g_usleep (100000);
      g_free (log);
      log = NULL;
      if (g_file_get_contents (logf, &log, NULL, NULL))
        {
          got5 = strstr (log, "move-sink-input 5 alsa_output.pci-0") != NULL;
          got9 = strstr (log, "move-sink-input 9 alsa_output.pci-0") != NULL;
        }
    }
  g_free (log);
  g_remove (logf);
  g_assert_true (got5);
  g_assert_true (got9);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  /* PATH restrito ao fakebin: probe determinístico, sem amixer. */
  g_setenv ("PATH", "tests/fakebin", TRUE);
  g_test_add_func ("/audio/backend_probe", test_backend_probe);
  g_test_add_func ("/audio/volume_icon", test_volume_icon);
  g_test_add_func ("/audio/dev_new", test_dev_new);
  g_test_add_func ("/audio/monitor_sources", test_monitor_sources);
  g_test_add_func ("/audio/descriptions", test_descriptions);
  g_test_add_func ("/audio/get_default", test_get_default);
  g_test_add_func ("/audio/list_sinks", test_list_sinks);
  g_test_add_func ("/audio/list_sources", test_list_sources);
  g_test_add_func ("/audio/move_streams", test_move_streams);
  return g_test_run ();
}
