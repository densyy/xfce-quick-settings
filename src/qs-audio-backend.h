/* qs-audio-backend.h — Audio pactl/wpctl backend (movido de qs-audio.c).
 * Movimento puro; UI (build/refresh/threads/idles) permanece em qs-audio.c.
 * NOTA: plano citava "move_streams_to_target" — nome real verificado no
 * arquivo é pactl_move_streams (def qs-audio.c:438, callers 483/508). */

#ifndef QS_AUDIO_BACKEND_H
#define QS_AUDIO_BACKEND_H

#include "qs-audio.h"

AudioBackend  audio_get_backend_cached  (void);
gboolean      audio_have_amixer_avail   (void);
void          set_volume_percent        (gint value);
void          set_source_volume_percent (gint value);
const gchar  *volume_icon_for_level     (gint percent);
void          audio_dev_free            (gpointer data);
AudioDev     *audio_dev_new             (const gchar *id,
                                        const gchar *label,
                                        gboolean     is_default);
GHashTable   *parse_pactl_monitor_sources (const gchar *output);
GHashTable   *parse_pactl_descriptions     (const gchar *output,
                                            const gchar *block_prefix);
gchar        *pactl_get_default         (const gchar *cmd);
GList        *list_pactl_sinks          (void);
GList        *list_pactl_sources        (void);
GList        *list_sinks                (AudioBackend backend);
GList        *list_sources              (AudioBackend backend);
void          pactl_move_streams        (const gchar *short_list_cmd,
                                        const gchar *move_cmd,
                                        const gchar *target);

#endif /* QS_AUDIO_BACKEND_H */
