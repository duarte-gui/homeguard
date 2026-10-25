#!/bin/bash
# ====================================================================================
# Publica um release do firmware no GitHub, de onde o homeguard-admin baixa (tecla d).
#
#   bash tools/release.sh                    # notas geradas pelos commits
#   bash tools/release.sh notas.md           # notas de um arquivo
#
# A versão sai do HG_FW_VERSION do homeguard.ino (tag v<versão>). Exige a árvore
# limpa e o commit já no GitHub: o release aponta para ele.
# O arquivo anexado se chama sempre homeguard.ino.bin — é por esse nome que o
# homeguard-admin procura.
# ====================================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "Erro: $*" >&2; exit 1; }

VER=$(sed -n 's/^#define HG_FW_VERSION "\(.*\)"/\1/p' homeguard.ino)
[ -n "$VER" ] || die "não achei o HG_FW_VERSION no homeguard.ino"
TAG="v$VER"
NOTES="${1:-}"

command -v gh >/dev/null || die "precisa do gh (GitHub CLI) logado"
command -v arduino-cli >/dev/null || die "precisa do arduino-cli"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "há mudanças sem commit"
git fetch -q origin
BRANCH=$(git rev-parse --abbrev-ref HEAD)
[ "$(git rev-parse HEAD)" = "$(git rev-parse "origin/$BRANCH" 2>/dev/null)" ] || die "dê push no $BRANCH antes"
gh release view "$TAG" >/dev/null 2>&1 && die "o release $TAG já existe (suba o HG_FW_VERSION)"

echo "--- interface web"
(cd data && { [ -d node_modules ] || npm install; } && npm run build)
python3 tools/embed_web.py

echo "--- firmware $VER"
BOARD=$(python3 -c "import json;print(json.load(open('.vscode/arduino.json'))['board'])")
OPTS=$(python3 -c "import json;print(json.load(open('.vscode/arduino.json')).get('configuration',''))")
BUILD=$(mktemp -d -t homeguard-release-XXXXXX)
trap 'rm -rf "$BUILD"' EXIT
arduino-cli compile --fqbn "$BOARD:$OPTS" --build-path "$BUILD" . | tail -3
BIN="$BUILD/homeguard.ino.bin"
grep -qa "HOMEGUARD_FW_VERSION=$VER" "$BIN" || die "o .bin não tem a marca da versão $VER"

# O homeguard-admin grava bootloader e partições embutidos nele: se a tabela de
# partições mudou, ele precisa ser atualizado antes.
cmp -s "$BUILD/homeguard.ino.partitions.bin" tools/homeguard-admin/assets/partitions.bin \
  || die "a tabela de partições mudou: copie o novo partitions.bin para tools/homeguard-admin/assets/"

echo "--- publicando $TAG"
ARGS=(--title "Firmware $VER" --target "$(git rev-parse HEAD)")
if [ -n "$NOTES" ]; then ARGS+=(--notes-file "$NOTES"); else ARGS+=(--generate-notes); fi
gh release create "$TAG" "$BIN#homeguard.ino.bin" "${ARGS[@]}"
echo "Pronto: o homeguard-admin já oferece o $TAG (abas Sistema e Gravar USB, tecla d)."
