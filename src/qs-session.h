#ifndef QS_SESSION_H
#define QS_SESSION_H

#include "qs-core.h"

/* Builds the Power tile + sub-page (shut down, restart, ...). */
void session_build_section (GtkWidget *box, GtkWidget *stack, QuickSettings *qs);

#endif /* QS_SESSION_H */
