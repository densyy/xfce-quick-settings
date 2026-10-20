#ifndef QS_PLUGIN_H
#define QS_PLUGIN_H

#include "qs-core.h"

/* Testable core of the panel plugin, independent of XfcePanelPlugin.
 * The XFCE entry points in qs-plugin.c are thin wrappers around these. */
void qs_plugin_build (GtkWidget      *container,
                      QuickSettings **out);
void qs_plugin_toggle_popup (QuickSettings *qs);
gboolean qs_plugin_size_changed (QuickSettings *qs,
                                 GtkWidget     *container,
                                 gint           size,
                                 gint           icon_size,
                                 gint           nrows);
void qs_plugin_teardown (QuickSettings *qs);

#endif /* QS_PLUGIN_H */
