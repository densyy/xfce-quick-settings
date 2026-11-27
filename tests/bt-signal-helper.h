/* bt-signal-helper.h — emissor deterministico de PropertiesChanged.
 *
 * Atalho sobre bt_signal_test_emit (src/qs-bt-signal.h, TASK-002): monta o
 * object path dev_MAC e o dicionario a{sv} com Connected e invoca o hook
 * de forma sincrona, sem barramento real, sem loop principal e sem D-Bus.
 * Por ser sincrono, o evento pode ser emitido com o verbo ainda em
 * execucao (entre o spawn do connect/disconnect e o fim do verbo), o que
 * permite a TASK-005 cobrir a corrida spawn, subscribe, evento sem
 * flakiness e sem display.
 *
 * Contrato de ambiente (padrao QS_FAKE_* do repo, AGENTS.md secao 2):
 *   QS_FAKE_BT_SIGNAL=Connected=true|Connected=false
 *     descreve o evento a emitir via bt_helper_emit_from_env; tambem
 *     aceita true|false nus; qualquer outro valor nao emite nada.
 *   QS_FAKE_BT_SIGNAL_PATH=/org/bluez/hci0/dev_11_22_33_44_55_66
 *     object path explicito do evento; quando ausente ou vazio, o path e
 *     derivado do MAC com o adaptador hci0.
 *   QS_FAKE_BT_SIGNAL_SENDER=:1.fake
 *     nome do dono no barramento, so repassado ao callback; quando
 *     ausente ou vazio usa ":1.fake".
 * O fake tests/fakebin/bluetoothctl nunca emite sinal: ele so persiste
 * estado (QS_FAKE_BT_STATE) e loga argv (QS_FAKE_LOG); o evento sempre
 * vem deste helper.
 *
 * Uso: inclua "bt-signal-helper.h" no teste e chame uma das funcoes com
 * um watch criado por bt_signal_watch_new_for_test. Header-only (static
 * inline): nenhum alvo de build muda e tests/run-tests.sh continua
 * compilando cada tests/test-*.c como antes.
 */
#ifndef BT_SIGNAL_HELPER_H
#define BT_SIGNAL_HELPER_H

#include <gio/gio.h>

#include "qs-bt-signal.h"

/* Deriva /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF a partir do MAC com
 * dois-pontos. Retorna string nova; o chamador libera com g_free.
 * Retorna NULL quando mac e NULL. */
static inline gchar *
bt_helper_object_path_for_mac (const gchar *mac)
{
  gchar *flat;
  gchar *path;
  gchar *p;

  if (mac == NULL)
    return NULL;
  flat = g_strdup (mac);
  for (p = flat; *p != '\0'; p++)
    {
      if (*p == ':')
        *p = '_';
    }
  path = g_strdup_printf ("/org/bluez/hci0/dev_%s", flat);
  g_free (flat);
  return path;
}

/* Monta o dicionario a{sv} com a chave Connected. Retorna referencia
 * nova; o chamador libera com g_variant_unref apos o emit. */
static inline GVariant *
bt_helper_connected_dict (gboolean connected)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "Connected",
                         g_variant_new_boolean (connected));
  return g_variant_builder_end (&builder);
}

/* Monta um dicionario a{sv} vazio (sem Connected): o casamento puro
 * classifica como BT_SIGNAL_NO_MATCH. Referencia nova como acima. */
static inline GVariant *
bt_helper_empty_dict (void)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  return g_variant_builder_end (&builder);
}

/* Emite Connected no object path dado, com sender ":1.fake". Nao faz
 * nada quando watch ou object_path e NULL. */
static inline void
bt_helper_emit_connected_at (BtSignalWatch *watch,
                             const gchar   *object_path,
                             gboolean       connected)
{
  GVariant *dict;

  if (watch == NULL || object_path == NULL)
    return;
  dict = bt_helper_connected_dict (connected);
  bt_signal_test_emit (watch, object_path, dict, ":1.fake");
  g_variant_unref (dict);
}

/* Emite Connected no path derivado do MAC (hci0). Nao faz nada quando
 * watch ou mac e NULL. */
static inline void
bt_helper_emit_connected (BtSignalWatch *watch,
                          const gchar   *mac,
                          gboolean       connected)
{
  gchar *path;

  if (watch == NULL || mac == NULL)
    return;
  path = bt_helper_object_path_for_mac (mac);
  bt_helper_emit_connected_at (watch, path, connected);
  g_free (path);
}

/* Emite um dicionario sem Connected no path derivado do MAC: exercita o
 * ramo NO_MATCH do casamento. Nao faz nada com watch ou mac NULL. */
static inline void
bt_helper_emit_without_connected (BtSignalWatch *watch,
                                   const gchar   *mac)
{
  gchar    *path;
  GVariant *dict;

  if (watch == NULL || mac == NULL)
    return;
  path = bt_helper_object_path_for_mac (mac);
  dict = bt_helper_empty_dict ();
  bt_signal_test_emit (watch, path, dict, ":1.fake");
  g_variant_unref (dict);
  g_free (path);
}

/* Emite o evento descrito por QS_FAKE_BT_SIGNAL no path de
 * QS_FAKE_BT_SIGNAL_PATH (ou derivado do MAC) com o sender de
 * QS_FAKE_BT_SIGNAL_SENDER. Retorna TRUE quando emitiu e FALSE quando
 * nao ha evento descrito (variavel ausente, vazia ou com valor
 * desconhecido) ou quando watch ou mac e NULL. */
static inline gboolean
bt_helper_emit_from_env (BtSignalWatch *watch,
                         const gchar   *mac)
{
  const gchar *spec;
  const gchar *path_env;
  const gchar *sender_env;
  gchar       *path = NULL;
  gboolean     connected = FALSE;
  gboolean     known = FALSE;
  GVariant    *dict;

  if (watch == NULL || mac == NULL)
    return FALSE;
  spec = g_getenv ("QS_FAKE_BT_SIGNAL");
  if (spec == NULL || *spec == '\0')
    return FALSE;
  if (g_ascii_strcasecmp (spec, "Connected=true") == 0
      || g_ascii_strcasecmp (spec, "true") == 0)
    {
      connected = TRUE;
      known = TRUE;
    }
  else if (g_ascii_strcasecmp (spec, "Connected=false") == 0
           || g_ascii_strcasecmp (spec, "false") == 0)
    {
      connected = FALSE;
      known = TRUE;
    }
  if (!known)
    return FALSE;
  path_env = g_getenv ("QS_FAKE_BT_SIGNAL_PATH");
  if (path_env != NULL && *path_env != '\0')
    path = g_strdup (path_env);
  else
    path = bt_helper_object_path_for_mac (mac);
  sender_env = g_getenv ("QS_FAKE_BT_SIGNAL_SENDER");
  if (sender_env == NULL || *sender_env == '\0')
    sender_env = ":1.fake";
  dict = bt_helper_connected_dict (connected);
  bt_signal_test_emit (watch, path, dict, sender_env);
  g_variant_unref (dict);
  g_free (path);
  return TRUE;
}

#endif /* BT_SIGNAL_HELPER_H */
