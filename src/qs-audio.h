#ifndef QS_AUDIO_H
#define QS_AUDIO_H

#include "qs-core.h"

typedef enum
{
  AUDIO_NONE = 0,
  AUDIO_PACTL,
  AUDIO_WPCTL
} AudioBackend;

/* One audio device: combo id + visible label + default flag. */
typedef struct
{
  gchar    *id;
  gchar    *label;
  gboolean  is_default;
} AudioDev;

/* Builds the Volume slider + devices tile on the shared grid (rows 0-1),
 * plus the Audio devices sub-page (output/input selectors). */
void audio_build_section (GtkWidget *grid, GtkWidget *stack, QuickSettings *qs);
/* Refresh slider position/icon from the current system volume. */
void audio_refresh_volume (QuickSettings *qs);
/* Reload output/input device lists and select the current defaults. */
void audio_refresh_devices (QuickSettings *qs);

#endif /* QS_AUDIO_H */
