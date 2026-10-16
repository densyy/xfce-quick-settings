#!/bin/sh
# Smoke test do plugin Quick Settings (sem instalar no sistema).
# 1) Verificação automática: dlopen do módulo + símbolo de registro do painel.
# 2) Roteiro manual documentado (ver final).
set -eu
cd "$(dirname "$0")/.."

SO=build/libquicksettings.so
if [ ! -f "$SO" ]; then
  echo "ERRO: $SO não existe — rode 'make build' antes." >&2
  exit 1
fi

echo "== 1) dlopen de $SO"
python3 - "$SO" <<'PY'
import ctypes, sys

lib = ctypes.CDLL(sys.argv[1])
sym = None
for name in ("xfce_panel_module_construct", "xfce_panel_module_preinit",
             "xfce_panel_module_new"):
    if hasattr(lib, name):
        sym = name
        break
if sym is None:
    raise SystemExit("ERRO: símbolo de módulo do painel não encontrado")
print("dlopen OK; símbolo presente:", sym)
PY

echo "== 2) símbolos de módulo exportados"
nm -D "$SO" | grep -i "xfce_panel_module" || true

cat <<'EOF'

== 3) roteiro manual (sessão gráfica XFCE) ==
  1. Instalar/testar: sh install/common/desktop.sh (instala e recarrega o painel)
     — ou instalar manualmente o .so + .desktop gerado.
  2. xfce4-panel -r (se não recarregou sozinho).
  3. Verificar: botão aparece no painel; clique abre o popup.
  4. Popup: navegar por Áudio, Wi-Fi, Bluetooth, Monitor, Notificações.
  5. Clique-direito no botão: menu de contexto do painel (Propriedades/Sobre).
  6. Redimensionar o painel (16–128 px) — ícone/borda seguem a HIG.
  7. journalctl -f -o cat | grep -i quick  → sem warnings inesperados.
  8. (Opcional, com cppcheck/valgrind no PATH) make cppcheck; make analyze.
EOF
