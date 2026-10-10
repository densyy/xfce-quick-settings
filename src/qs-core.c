#include "qs-core.h"

void
qs_ref (QuickSettings *qs)
{
  g_atomic_int_inc (&qs->refcount);
}

void
qs_unref (QuickSettings *qs)
{
  if (g_atomic_int_dec_and_test (&qs->refcount))
    {
      g_free (qs->bt_controller);
      g_free (qs->sysmon_disk_key);
      g_free (qs->display_last);
      g_free (qs);
    }
}
