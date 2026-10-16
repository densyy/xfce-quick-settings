/* qs-brightness.h — testabilidade do parse de output xrandr.
 * bright_extract_output era static em qs-brightness.c; exposta sem
 * mudar lógica (precedente M13: bright_stop declarada em qs-display.h). */

#ifndef QS_BRIGHTNESS_H
#define QS_BRIGHTNESS_H

#include <glib.h>

/* Primeiro output " connected" de um dump xrandr (para cache do setter).
 * Caller frees. */
gchar *bright_extract_output (const gchar *dump);

#endif /* QS_BRIGHTNESS_H */
