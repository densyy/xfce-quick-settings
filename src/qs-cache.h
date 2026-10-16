#ifndef QS_CACHE_H
#define QS_CACHE_H
#include "qs-core.h"
#define QS_CACHE_TTL_FAST_S 15    /* wifi ssid/conectado, bt conectado, mute */
#define QS_CACHE_TTL_SLOW_S 300   /* wifi on/off, bt powered, brilho, volume, bateria */
#define QS_CACHE_DEBOUNCE_MS 500
void qs_cache_load (QuickSettings *qs);        /* main thread; aplica nos widgets */
void qs_cache_save (QuickSettings *qs);        /* main thread; escrita atômica */
void qs_cache_schedule_save (QuickSettings *qs); /* debounce 500ms via g_timeout_add */
void qs_cache_stop (void);                     /* cancela debounce pendente (teardown) */
#endif
