#ifndef QS_BT_SIGNAL_H
#define QS_BT_SIGNAL_H

#include "qs-core.h"

/* Seam GDBus para o sinal PropertiesChanged de org.bluez.Device1.
 *
 * O módulo assina SEM filtro de object path e repassa o evento cru
 * (caminho recebido, dicionário de propriedades e nome do dono) ao
 * callback do link. O casamento do evento com o dispositivo (sufixo
 * dev_MAC do caminho mais valor de Connected) é papel do link via
 * a função pura da TASK-001 na TASK-003; este módulo nunca casa.
 *
 * Contrato de memória: changed_properties é emprestado pela duração
 * do callback (pertence ao GDBus ou ao chamador de
 * bt_signal_test_emit); o callback não deve reter nem liberar o
 * dicionário. object_path e sender_name também são emprestados e o
 * callback deve tolerar NULL. */

typedef struct BtSignalWatchPrivate BtSignalWatch;

/* Callback do link: caminho recebido na mensagem, dicionário de
 * propriedades alteradas (a{sv}) e nome do dono no barramento. */
typedef void (*BtSignalChangedCb) (const gchar *object_path,
                                   GVariant    *changed_properties,
                                   const gchar *sender_name,
                                   gpointer     user_data);

/* Assina PropertiesChanged (org.freedesktop.DBus.Properties, arg0
 * igual a org.bluez.Device1) no barramento do sistema, sem filtro de
 * object path. Retorna o watch, ou NULL com error preenchido quando
 * o barramento está ausente; nunca aborta o processo. O chamador
 * libera com bt_signal_unsubscribe. */
BtSignalWatch *bt_signal_subscribe (BtSignalChangedCb  cb,
                                    gpointer           user_data,
                                    GError           **error);

/* Watch sem barramento para testes: nunca toca o D-Bus e sempre
 * funciona; combine com bt_signal_test_emit. Libere com
 * bt_signal_unsubscribe como um watch normal. */
BtSignalWatch *bt_signal_watch_new_for_test (BtSignalChangedCb cb,
                                             gpointer          user_data);

/* Ponteiro de subscribe injetável: a TASK-003 chama este ponteiro em
 * vez de bt_signal_subscribe direto, e os testes podem trocá-lo para
 * observar a assinatura e devolver um watch de teste. Passar NULL em
 * set restaura o padrão (bt_signal_subscribe). */
typedef BtSignalWatch *(*BtSignalSubscribeFn) (BtSignalChangedCb  cb,
                                               gpointer           user_data,
                                               GError           **error);
extern BtSignalSubscribeFn bt_signal_subscribe_fn;
void bt_signal_set_subscribe_fn (BtSignalSubscribeFn fn);

/* Simula o evento sem barramento: invoca o callback do watch de
 * forma síncrona com os valores dados. Funciona com watch real ou de
 * teste, inclusive com o verbo ainda em execução, pois não depende do
 * loop principal nem do barramento. NULL-safe em todos os
 * parâmetros; sem o callback ou sem o dicionário, nada acontece. */
void bt_signal_test_emit (BtSignalWatch *watch,
                          const gchar   *object_path,
                          GVariant      *changed_properties,
                          const gchar   *sender_name);

/* Cancela a assinatura e libera o watch. Idempotente: aceita NULL e
 * watch com id zero (inclui watch de teste); nunca aborta. Após a
 * chamada o watch não pode mais ser usado e o chamador deve anular o
 * próprio ponteiro, o que torna o unsubscribe duplo seguro. */
void bt_signal_unsubscribe (BtSignalWatch *watch);

#endif /* QS_BT_SIGNAL_H */
