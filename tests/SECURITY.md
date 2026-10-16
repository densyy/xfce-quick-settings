# Notas de segurança — M09 + E2E (quick-settings)

Cobertura automatizada das decisões de quoting (item M09 do plano).
Execução: `sh tests/run-tests.sh` (grupos fuzz/security) e
`sh tests/run-sanitizers.sh` (ASan/LSan/UBSan sobre toda a suite).

## 1. Quoting verificado no código (M09)

| Ponto | Comando | Mecanismo | Evidência de teste |
|---|---|---|---|
| `qs-session.c` logout | `loginctl terminate-user %s` | `g_shell_quote (g_get_user_name ())` | `test-e2e` session_logout: log contém username quotado |
| `qs-bt-backend.c` show/info/devices | `bluetoothctl ... %s` (MAC/controller/filter) | `g_shell_quote` nos 3 pontos | `test-bt` + fuzz `bt_evil` (MAC com `;`, espaços, `$()`) |
| `qs-bluetooth.c` link/verify/repair/toggle | `bluetoothctl --timeout N ... %s` | `g_shell_quote (l->mac)` nos 4 pontos | `test-e2e` bt_toggle_info: MAC `AA:BB:CC:DD:EE:FF` limpo no log |
| `qs-wifi.c` connect | `nmcli device wifi connect %s` (+password) | `g_shell_quote (ssid)` / senha quotada | `test-wifi-security` connect_quotes_ssid/specials: SSID rogue com `` ` ``/`$()`/`;` → argv íntegro, marker de pwn ausente |
| `set_default_sink/source` (pactl) | `pactl set-default-sink %s` + move streams | `g_shell_quote (id)` | `test-e2e` audio_sink: id com espaço chega quotado |
| `on_mode_changed` | `xrandr --output %s --mode %s` (SEM aspas) | allowlist estrita: `valid_monitor_shared` ([A-Za-z0-9_.-], ≤64) + `valid_mode` (dígitos+x+dígitos) — metacaracteres de shell impossíveis | `test-e2e` display_mode: comando exato `xrandr --output DP-1 --mode 1024x768`; `test-monitor-data` cobre os validadores |
| `on_scale_changed` | `xrandr --output %s --scale %sx%s` | ids validados (`valid_monitor_shared`, `valid_scale`) + `g_shell_quote (mon)` | `test-e2e` display_scale: `--scale 1.5x1.5` emitido |

Exceção consciente (baixo risco): `wpctl set-default %s` sem quote —
o id vem de `parse_wpctl_section` via `strtol` (só dígitos). Cobertura:
`test-fuzz` inclui seção wpctl adversarial.

## 2. Metodologia dos asserts de quoting

O fake loga `$*` **pós-shell** (aspas somem no log), então a prova é
dupla e está em `test-wifi-security.c` / `test-e2e.c`:

1. `argc=N` exato no log (argumento com espaços/`;` conta como UM);
2. arquivo-marcador de pwn (`/tmp/qs-pwned-*`) AUSENTE após o fluxo.

Ex.: SSID `Evil;touch /tmp/qs-pwned-wifi` → `nmcli argc=4` +
`cmd=device wifi connect Evil...` (argumento único) + marker ausente =
quoting OK. Sem quote, o `;` quebraria o comando (argc maior) e o
marker existiria.

## 3. Superfície adversarial coberta pelo fuzz (`test-fuzz.c`)

- `parse_wifi_list`: SSIDs com `:`, `\`, unicode, linhas de 64KB,
  10k redes, campos faltando/sobrando, `%n`/`%s` (format-string).
- `bt_output_is_error`: combinações "Failed"+"successful", NULL/NULL.
- `unescape_mount`: `\` final, `\999` inválido, sequência longa.
- `qs_parse_int_checked` / `split_terse_line` / `parse_first_percent`:
  overflow, sinais, lixo, `%` solto, 1MB de dígitos.
- `bright_extract_output`, `valid_monitor_shared`, `flat_text`,
  `volume_icon_for_level`: escapes, nomes hostis, chars de controle.
- SQLite maligno (`test-notify-store.c`): DB com tabela ausente,
  tipos errados, timestamps absurdos, 35 linhas (LIMIT 30).

## 4. Sanitizers (valgrind indisponível nesta VM)

O valgrind do sistema quebra (glibc CachyOS com ld.so stripped) —
ver `RELATORIO.md` da fase de pesquisa. Substituição:

- `tests/run-sanitizers.sh`: recompila TODOS os `test-*.c` com
  `-fsanitize=address,undefined -fno-sanitize-recover=all`
  (ASan+LSan+UBSan) e roda a suite inteira. Leak check do LSan
  cobre `qs_ref`/`qs_unref` por domínio, frees de listas/hashes e
  paths de erro dos parsers.
- UBSan cobre os corpus do fuzz (shifts, overflow, NULL) em cada run.
