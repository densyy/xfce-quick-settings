#ifndef QS_BT_AGENT_H
#define QS_BT_AGENT_H

#include "qs-core.h"

/* Agente BlueZ org.bluez.Agent1 restrito ao alvo (D-015 RF-005).
 *
 * O agente e registrado sob demanda no inicio do verbo do usuario
 * (pair, trust ou connect) com o object path do dispositivo alvo
 * derivado do MAC, e liberado ao fechar o link. Durante a janela de
 * registro, pedidos sem PIN do alvo recebem accept automatico e
 * pedidos de qualquer outro device path recebem o erro
 * org.bluez.Error.Rejected sem dialogo. Quando o BlueZ exige PIN
 * para o alvo, um dialogo GTK modal coleta o codigo.
 *
 * Comparacao alvo/pedido: apenas o sufixo dev_XX apos a ultima
 * barra e comparado, sem diferenciar maiusculas, de modo que o
 * prefixo do adaptador (hci0, hci1, ...) e ignorado, como em
 * bt_signal_event_matches. Mesma regra vale para o MAC com dois
 * pontos e o caminho com sublinhados.
 *
 * Contrato de memoria: o alvo e copiado no registro; a matriz
 * get_target e emprestada pelo agente; device_path_for_mac e
 * ask_pin devolvem copias que o chamador libera. O GtkWindow do
 * registro e emprestado e pode ser NULL. Registro sem barramento
 * do sistema devolve NULL com error preenchido e nunca aborta. */

#define QS_BT_AGENT_OBJECT_PATH "/org/xfce/QuickSettings/Agent1"
#define QS_BT_AGENT_REJECTED "org.bluez.Error.Rejected"
#define QS_BT_AGENT_CAPABILITY "KeyboardDisplay"

typedef struct QsBtAgent QsBtAgent;

/* Devolve o object path canonico do dispositivo para o MAC dado
 * (forma /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF, com hex em caixa
 * alta), ou NULL quando o MAC nao tem a forma AA:BB:CC:DD:EE:FF
 * com 6 pares hexadecimais. O chamador libera o retorno. */
gchar *qs_bt_agent_device_path_for_mac (const gchar *mac);

/* TRUE quando request_path pertence ao alvo do agente, pelo sufixo
 * dev_XX apos a ultima barra, sem diferenciar maiusculas. NULL-safe
 * em todos os parametros; devolve FALSE sem alvo ou sem sufixo.
 * Funcao pura sobre strings, testavel sem barramento. */
gboolean qs_bt_agent_path_matches (const gchar *target_path,
                                   const gchar *request_path);

/* Igual a path_matches com o alvo guardado no agente. NULL-safe;
 * devolve FALSE sem agente ou sem alvo registrado. */
gboolean qs_bt_agent_is_target (const QsBtAgent *agent,
                                const gchar     *request_path);

/* Alvo canonico guardado no registro, emprestado pelo agente, ou
 * NULL quando o agente e NULL. */
const gchar *qs_bt_agent_get_target (const QsBtAgent *agent);

/* Dialogo GTK modal que coleta o PIN do alvo. Devolve copia do
 * codigo digitado, ou NULL quando o agente e NULL, quando nao ha
 * display, quando o dialogo e cancelado ou quando o campo esta
 * vazio. O chamador libera o retorno. Nunca aborta. */
gchar *qs_bt_agent_ask_pin (QsBtAgent *agent,
                            GtkWindow *parent);

/* Registra o objeto Agent1 no barramento do sistema com o alvo
 * derivado de mac e anuncia o agente ao AgentManager1 do BlueZ.
 * Devolve o agente, ou NULL com error preenchido quando o MAC e
 * invalido, quando o barramento esta ausente ou quando o registro
 * no BlueZ falha; nunca aborta o processo. */
QsBtAgent *qs_bt_agent_register (const gchar  *mac,
                                 GtkWindow    *parent,
                                 GError      **error);

/* Libera o agente do AgentManager1, remove o objeto do barramento
 * e libera a memoria. NULL-safe; apos a chamada o agente e
 * invalido e o chamador deve anular o proprio ponteiro. */
void qs_bt_agent_unregister (QsBtAgent *agent);

#endif /* QS_BT_AGENT_H */
