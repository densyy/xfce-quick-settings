# Notas de segurança e cobertura de testes (quick-settings)

Este documento registra o que a suíte atual verifica sobre quoting e
isolamento de comandos externos. As afirmações foram conferidas contra os
testes e fakes desta árvore; lacunas conhecidas estão marcadas como
**não coberto**. Não trate a tabela como garantia de cobertura integral.

Execução: `sh tests/run-tests.sh`. Sanitizers:
`sh tests/run-sanitizers.sh`.

## 1. Quoting verificado no código

| Ponto | Comando | Mecanismo | Cobertura atual |
|---|---|---|---|
| `qs-session.c` logout (XFCE) | `xfce4-session-logout --logout` | string fixa, sem interpolação | `test-e2e` `session_logout` clica no botão e exige `--logout` no log do fake. Depende de display; sem display o teste é skipado. |
| `qs-session.c` logout (fallback) | `loginctl terminate-user %s` | `g_shell_quote (g_get_user_name ())` | **Não coberto.** O E2E tem `tests/fakebin/xfce4-session-logout` no PATH, então o ramo `loginctl` nunca é tomado. `tests/fakebin/loginctl` existe, mas nenhum teste o aciona. |
| `qs-session.c` halt/reboot/hibernate/suspend | `xfce4-session-logout --halt` etc.; fallback `systemctl poweroff` etc. | strings fixas, sem dados interpolados | **Não coberto.** `test-ui` só verifica que os cinco botões existem; nenhum teste clica nesses quatro. Não há fake de `systemctl` em `tests/fakebin`. |
| `qs-bt-backend.c` show/info/devices | `bluetoothctl --timeout 1 show %s`, `info %s`, `devices %s` | `g_shell_quote` no controller, no MAC e no filtro | `test-bt` cobre controller/list/info/devices com MAC fixo e `test-net` cobre `show AA:BB:CC:DD:EE:FF`. **Não há caso adversarial de MAC ou filtro**; o fuzz cobre apenas a classificação de erros. |
| `qs-bluetooth.c` toggle/verify | `bluetoothctl --timeout 1 info %s`, `--timeout 10 %s %s` | `g_shell_quote (l->mac)` nos quatro pontos | `test-e2e` `bt_toggle_info` verifica o primeiro `info`; `test-bluetooth-flow` percorre a cadeia com MACs fixos. **Sem MAC hostil**; o quoting é verificado por inspeção do código. |
| `qs-wifi.c` connect | `nmcli connection up %s`, `nmcli device wifi connect %s` (+ `password %s`) | `g_shell_quote` no SSID e na senha | `test-wifi-security` `connect_quotes_ssid`/`specials`: SSID com `;`, `$()` e crase chega como um único argv (`argc=4`) e os markers `/tmp/qs-sec-wifi*` ficam ausentes. `test-wifi-flow` cobre senha com valor simples. |
| `set_default_sink/source` (pactl) | `pactl set-default-sink %s`/`set-default-source %s` + move streams | `g_shell_quote (id)` | `test-audio-flow` verifica IDs exatos (`bluez_output.AB`, `alsa_input.pci-0`) e os comandos de move; `test-e2e` `audio_sink` verifica `test-sink`. **Não há caso com espaço ou metacaractere no id**, então o quote não é provado por teste. |
| `on_mode_changed` | `xrandr --output %s --mode %s` | allowlist `valid_monitor_shared` + `valid_mode` e `g_shell_quote` nos dois valores | `test-e2e` `display_mode` exige o comando exato `xrandr --output DP-1 --mode 1024x768`; `test-monitor-data` cobre os validadores. |
| `on_scale_changed` | `xrandr --output %s --scale %sx%s` | `valid_monitor_shared` + `valid_scale`; `g_shell_quote (mon)` e fator validado | `test-e2e` `display_scale` exige `--scale 1.5x1.5`; `test-monitor-data` cobre os validadores. |
| `wpctl set-default %s` | `wpctl set-default %s` | sem quote; o id vem de `parse_wpctl_section` via `strtol` (apenas dígitos) | **Cobertura apenas de ids válidos** (`test-audio-wpctl`, ids 57/58). `test-fuzz` não tem seção wpctl adversarial. |

## 2. Metodologia dos asserts de quoting

O fake de `test-wifi-security.c` e o nmcli de `test-e2e.c` registram
`argc=N cmd=...`. Como o shell remove as aspas antes do fake, o log não
mostra o quoting literal; a prova é dupla:

1. `argc=N` exato no log (um argumento com espaços ou `;` conta como UM);
2. arquivo-marcador de pwn ausente após o fluxo.

Exemplo: o SSID `Evil"; touch /tmp/qs-sec-wifi; echo "` produz
`nmcli argc=4 cmd=device wifi connect Evil...` (argumento único) e o marker
`/tmp/qs-sec-wifi` não existe. Sem quote, o `;` quebraria o comando (argc
maior) e o marker seria criado. O segundo caso usa
`/tmp/qs-sec-wifi2`. Os demais fakes, quando registram log, gravam
apenas `$*`; neles a prova é o argv exato esperado no log.

## 3. Isolamento de comandos externos e display

A regra dos testes de fluxo é prefixar `PATH` com `tests/fakebin` (ou
apontar `PATH` somente para um diretório fake dedicado), de modo que, para
os comandos exercitados, o primeiro match seja o fake. Testes que precisam
simular ferramenta ausente usam um diretório temporário vazio.
`QS_FAKE_LOG` recebe o log de argv. Os fakes nunca chamam a ferramenta real.

- **Wi-Fi:** `tests/fakebin/nmcli`, com estado por variáveis `QS_FAKE_*`.
  Usado por `test-wifi.c`, `test-wifi-flow.c`, `test-wifi-tile.c`,
  `test-net.c` e `test-e2e.c`. `test-wifi-security.c` cria um nmcli
  temporário próprio, que também registra `argc`.
- **Bluetooth:** `tests/fakebin/bluetoothctl`, com estado controlado por
  `QS_FAKE_BT_*`. Usado por `test-bt.c`, `test-bluetooth-flow.c`,
  `test-net.c` e `test-e2e.c`.
- **Áudio:** `tests/fakebin/pactl`, `tests/fakebin/wpctl` e
  `tests/fakebin/amixer`. `test-audio-wpctl.c` e `test-audio-amixer.c`
  copiam o fake para um diretório temporário exclusivo, garantindo que só
  aquele backend responda.
- **Brilho e nightlight:** `tests/fakebin/brightnessctl`,
  `tests/fakebin/xbacklight`, `tests/fakebin/xrandr` e
  `tests/fakebin/redshift`. Há cópias dedicadas em
  `tests/fakebin-xback/xbacklight`, `tests/fakebin-xrandr/xrandr` e
  `tests/fakebin-gammastep/gammastep`. `QS_BACKLIGHT_DIR` permite isolar
  uma árvore sysfs temporária; a leitura da árvore real
  `/sys/class/backlight` é somente leitura e o caso é skipado se não
  houver backlight legível.
- **Energia:** `tests/fakebin/upower` e `QS_POWER_SUPPLY_DIR` para uma
  árvore sysfs temporária. O caso `/power-flow/sysfs_real` lê
  `/sys/class/power_supply` real apenas em modo somente leitura; sem
  bateria, aceita o estado "sem energia". Nos testes, nenhuma escrita em
  sysfs real é feita; os caminhos de escrita usam backends falsos.
- **Sessão:** `tests/fakebin/xfce4-session-logout` e
  `tests/fakebin/loginctl` existem e registram argv. O E2E usa apenas o
  primeiro. **Não há `tests/fakebin/systemctl` nesta árvore**, portanto o
  fallback `systemctl` não é exercitado. A cobertura fake-based das ações
  de sessão ainda não está presente nesta árvore; qualquer cobertura
  futura deve usar um `systemctl` falso e PATH controlado, nunca o binário
  real.
- **UI sem display:** os testes que constroem widgets chamam
  `gtk_init_check` e, sem display, fazem `g_test_skip ("needs display")`.
  Os testes de posicionamento (`test-popup`, `test-plugin`) mapeiam uma
  janela temporária sem decoração e com opacidade zero, destruída ao final;
  os demais constroem widgets sem mapear um toplevel. Sem display, nenhum
  fluxo de UI roda.
- **Estado do usuário:** testes de notificações usam `XDG_CACHE_HOME` e
  `XDG_DATA_HOME` temporários, para não tocar no banco real do usuário.

## 4. Lacunas conhecidas

- Logout pelo fallback `loginctl terminate-user` (só o caminho
  `xfce4-session-logout` é coberto).
- Ações halt/reboot/hibernate/suspend, tanto pelo XFCE quanto pelos
  fallbacks `systemctl`.
- MAC ou filtro hostil nos caminhos do `bluetoothctl` (backend e UI).
- `wpctl set-default` com entrada adversarial.
- `pactl set-default-sink/source` com id contendo espaço ou
  metacaractere.
- Qualquer fluxo de UI quando não há display disponível.

## 5. Superfície adversarial coberta pelo fuzz (`test-fuzz.c`)

- `parse_wifi_list`: SSIDs com aspas, `;`, `$()`, crase, unicode, `:`
  extra, SSID vazio e uma lista de 5000 redes.
- `bt_output_is_error`: `test-fuzz` cobre maiúsculas/minúsculas, "Failed"
  benigno e `org.bluez.Error.AuthenticationFailed`; `test-bt.c`
  complementa com `NULL`/`NULL`, erro em stderr e "AlreadyExists".
- `unescape_mount`: `\` final, `\1`, `\999`, `\040`, `\134` e 1 MB de
  entrada com escapes.
- `qs_parse_int_checked` / `split_terse_line` / `parse_first_percent`:
  overflow, sinais, hexadecimal, dígitos unicode, 100 mil dígitos, campos
  de 10 KB e `%` solto.
- `bright_extract_output`, `valid_monitor_shared`, `flat_text` e
  `volume_icon_for_level`: nomes gigantes, `connectedish`, 1 MB de
  delimitadores e extremos de `int`.
- SQLite maligno: em `test-fuzz.c`, strings de 1 MB e timestamp com tipo
  errado; em `test-notify-store.c`, banco ausente, tabela ausente,
  35 linhas com LIMIT 30 e assinatura COUNT/MAX.

## 6. Sanitizers (valgrind indisponível nesta VM)

O `tests/run-sanitizers.sh` registra que o valgrind do sistema quebra
nesta VM (glibc CachyOS com `ld.so` stripped). Substituição:

- O script recompila os `test-*.c` com
  `-fsanitize=address,undefined -fno-sanitize-recover=all`
  (ASan+LSan+UBSan), exceto `test-perf.c` (timing não é significativo sob
  sanitizer), e roda a suíte.
- O leak check do LSan fica ativo na maioria dos binários. Ele é desligado
  em `test-cache`, `test-lifecycle`, `test-wifi-flow`,
  `test-wifi-security`, `test-wifi-tile` e `test-e2e`, que chamam
  `gtk_init` e deixam roots one-time do GLib/GIO; ASan e UBSan continuam
  ativos nesses casos.
- UBSan fica ativo em todos os binários, inclusive no corpus do fuzz;
  qualquer comportamento indefinido detectado aborta a execução.
