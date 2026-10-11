/* qs-notify-store.h — notification history store (sqlite) movido de qs-notify.c. */

#ifndef QS_NOTIFY_STORE_H
#define QS_NOTIFY_STORE_H

#include "qs-core.h"

#define NOTIFY_MAX_ROWS 30

typedef struct
{
  gchar *app;
  gchar *summary;
  gchar *body;
  gint64 timestamp;
} Notice;

void     notice_free          (gpointer data);
gchar   *notify_db_path       (void);
gchar   *flat_text            (const unsigned char *src);
GList   *list_notices         (void);
gchar   *notice_time          (gint64 timestamp_usec);
gboolean notify_db_signature  (gint   *count_out,
                               gint64 *max_out);

#endif /* QS_NOTIFY_STORE_H */
