#!/bin/sh
# Publica a release no GitHub: cria a tag vX.Y.Z (de QS_VERSION) e anexa
# o .so já buildado em release/x86_64-linux/.
#
# Uso: sh release/publish.sh
#
# Precisa de UMA das duas formas de autenticação:
#   1) `gh` instalado e autenticado (`gh auth login`), ou
#   2) variável GITHUB_TOKEN com um token com permissão `contents:write`.
#
# O install/desinstall local NÃO usa este script; ele só publica.
set -eu
cd "$(dirname "$0")/.."

VERSION="$(sed -n 's/^#define QS_VERSION "\(.*\)"$/\1/p' src/qs-core.h)"
if [ -z "$VERSION" ]; then
  echo "ERRO: QS_VERSION não encontrado em src/qs-core.h" >&2
  exit 1
fi
TAG="v$VERSION"
ASSET="release/x86_64-linux/libquicksettings.so"
if [ ! -f "$ASSET" ]; then
  echo "ERRO: $ASSET não existe — rode 'make build' antes." >&2
  exit 1
fi

# owner/repo a partir do remote origin (https ou ssh).
URL="$(git remote get-url origin)"
REPO="$(printf '%s' "$URL" | sed -e 's#^https://github.com/##' \
  -e 's#^git@github.com:##' -e 's#\.git$##')"
case "$REPO" in
  */*) ;;
  *) echo "ERRO: remote origin não é um repo GitHub: $URL" >&2; exit 1 ;;
esac

if command -v gh >/dev/null 2>&1; then
  echo "Publicando $TAG em $REPO via gh…"
  gh release create "$TAG" "$ASSET" \
    --repo "$REPO" \
    --title "Quick Settings $TAG" \
    --notes "Build x86_64-linux de libquicksettings.so ($TAG)." \
    --latest
  echo "OK: release $TAG publicada."
  exit 0
fi

if [ -z "${GITHUB_TOKEN:-}" ]; then
  echo "ERRO: nem 'gh' nem GITHUB_TOKEN disponíveis." >&2
  echo "Instale gh (gh auth login) ou exporte GITHUB_TOKEN." >&2
  exit 1
fi

echo "Publicando $TAG em $REPO via API…"
API="https://api.github.com/repos/$REPO/releases"
AUTH="Authorization: Bearer $GITHUB_TOKEN"
RESP="$(curl -sf -H "$AUTH" -H "Accept: application/vnd.github+json" \
  -d "{\"tag_name\":\"$TAG\",\"name\":\"Quick Settings $TAG\",\"body\":\"Build x86_64-linux de libquicksettings.so ($TAG).\",\"draft\":false,\"prerelease\":false}" \
  "$API")" || {
  echo "ERRO: falha ao criar a release (verifique o token e o repo)." >&2
  exit 1
}
UPLOAD_URL="$(printf '%s' "$RESP" | sed -n 's/.*"upload_url": *"\([^"]*\){.*/\1/p')"
if [ -z "$UPLOAD_URL" ]; then
  echo "ERRO: resposta da API sem upload_url." >&2
  exit 1
fi
curl -sf -H "$AUTH" -H "Content-Type: application/octet-stream" \
  --data-binary "@$ASSET" \
  "$UPLOAD_URL?name=$(basename "$ASSET")" > /dev/null \
  || { echo "ERRO: falha no upload do asset." >&2; exit 1; }
echo "OK: release $TAG publicada."
