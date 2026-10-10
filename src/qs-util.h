#ifndef QS_UTIL_H
#define QS_UTIL_H

#include "qs-core.h"

/* Async command: runs in a worker thread, done() fires in the main loop. */
typedef void (*QsCmdDone) (const gchar *output,
                           const gchar *errout,
                           gboolean     ok,
                           gpointer     user_data);

gboolean command_exists (const gchar *name);
/* Runs cmdline synchronously, returns stdout (caller frees) or NULL. */
gchar *run_command (const gchar *cmdline);
void spawn_command (const gchar *cmdline);
void run_command_async (const gchar *cmdline,
                        QsCmdDone    done,
                        gpointer     user_data);
/* qs-aware variant: holds a qs ref across the worker thread and guarantees
 * qs is alive while done() runs. done() still must check qs->dead before
 * touching widgets, and must NOT unref (ownership stays here).
 * For wrapper structs (first field = qs, e.g. WifiConnect/BtLink/MonPower)
 * keep using run_command_async with manual qs_ref/qs_unref. */
void run_command_async_qs (const gchar     *cmdline,
                           QsCmdDone        done,
                           QuickSettings   *qs);
/* Split an nmcli terse line on unescaped ':' (handles \: and \\). */
gchar **split_terse_line (const gchar *line,
                          gint         expected_fields);
/* First "NN%" found in text, or -1. */
gint parse_first_percent (const gchar *text);

#endif /* QS_UTIL_H */
