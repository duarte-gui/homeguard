#!/bin/bash

# ====================================================================================
# ESP32 Homeguard Flasher - detecta a placa e grava o binário certo
# ====================================================================================
#
# O script:
#   1. Descobre a porta serial sozinho (ou usa a que você passar).
#   2. Pergunta ao chip quem ele é (esptool) — ESP32, C3, S3, C6...
#   3. Compila o sketch para esse chip, se você não passar um build pronto.
#   4. CONFERE se o binário foi realmente compilado para o chip conectado
#      antes de gravar qualquer coisa.
#   5. Constrói a interface web e a imagem LittleFS.
#   6. Grava tudo com o offset de bootloader correto do chip (0x0 ou 0x1000).
#
# --- COMO USAR ---
#   bash flash.sh                          # detecta a porta e compila sozinho
#   bash flash.sh /dev/ttyACM0             # porta fixa, compila sozinho
#   bash flash.sh /dev/ttyACM0 /tmp/build  # usa um build já pronto (ex.: da IDE)
#
# --- PRÉ-REQUISITOS ---
#   - Node.js e npm (para a interface web).
#   - arduino-cli, se você não passar um diretório de build pronto.
#   - Usuário no grupo do device serial (uucp/dialout, conforme a distro).
#
# ====================================================================================

set -uo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BLUE='\033[0;36m'; NC='\033[0m'
die() { echo -e "\n${RED}Erro: $*${NC}" >&2; exit 1; }
step() { echo -e "\n${GREEN}--- $* ---${NC}"; }

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SERIAL_PORT="${1:-}"
BUILD_DIR="${2:-}"

# ====================================================================================
# FERRAMENTAS
# ====================================================================================
ESP32_PACKAGES_DIR="${ARDUINO15_DIR:-$HOME/.arduino15}/packages/esp32/tools"
: "${ESPTOOL_DIR:=$(ls -d "$ESP32_PACKAGES_DIR"/esptool_py/*/ 2>/dev/null | sort -V | tail -n 1)}"
: "${MKLITTLEFS_DIR:=$(ls -d "$ESP32_PACKAGES_DIR"/mklittlefs/*/ 2>/dev/null | sort -V | tail -n 1)}"
[ -n "$ESPTOOL_DIR" ] && [ -d "$ESPTOOL_DIR" ] || die "esptool_py não encontrado em $ESP32_PACKAGES_DIR"
[ -n "$MKLITTLEFS_DIR" ] && [ -d "$MKLITTLEFS_DIR" ] || die "mklittlefs não encontrado em $ESP32_PACKAGES_DIR"

if [ -x "${ESPTOOL_DIR}esptool" ]; then
    ESPTOOL="${ESPTOOL_DIR}esptool"
elif [ -f "${ESPTOOL_DIR}esptool.py" ]; then
    ESPTOOL="python3 ${ESPTOOL_DIR}esptool.py"
else
    die "nem 'esptool' nem 'esptool.py' foram encontrados em $ESPTOOL_DIR"
fi
MKLITTLEFS="${MKLITTLEFS_DIR%/}/mklittlefs"

# ====================================================================================
# PASSO 1: DESCOBRIR A PORTA SERIAL
# ====================================================================================
step "Passo 1: Localizando a porta serial"

if [ -z "$SERIAL_PORT" ]; then
    # /dev/serial/by-id é estável: não muda de nome conforme a ordem de plugagem.
    mapfile -t CANDIDATES < <(ls /dev/serial/by-id/* 2>/dev/null)
    if [ "${#CANDIDATES[@]}" -eq 0 ]; then
        mapfile -t CANDIDATES < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null)
    fi
    case "${#CANDIDATES[@]}" in
        0) die "nenhuma placa encontrada. Conecte o ESP e confira o cabo (tem que ser cabo de dados).";;
        1) SERIAL_PORT="${CANDIDATES[0]}";;
        *) echo -e "${YELLOW}Mais de uma placa conectada:${NC}"
           printf '  %s\n' "${CANDIDATES[@]}"
           die "passe a porta desejada: bash $0 <porta>";;
    esac
fi

[ -e "$SERIAL_PORT" ] || die "a porta '$SERIAL_PORT' não existe."
if [ ! -r "$SERIAL_PORT" ] || [ ! -w "$SERIAL_PORT" ]; then
    # -L segue o symlink: /dev/serial/by-id/* e' um link, e o stat sem -L devolveria
    # o grupo do proprio link (sempre 'root'), nao o do device.
    GRP=$(stat -Lc '%G' "$SERIAL_PORT")
    if [ "$GRP" = "root" ]; then
        die "sem permissão em '$SERIAL_PORT', e o device pertence ao grupo 'root'.
       Isso é incomum — confira a regra de udev da sua distro. NÃO se adicione ao
       grupo root para contornar."
    fi
    die "sem permissão em '$SERIAL_PORT' (o device pertence ao grupo '$GRP').
       Corrija com:  sudo usermod -aG $GRP \$USER
       Depois faça LOGOUT/LOGIN. Os grupos de um processo são fixados quando ele
       nasce e nunca são reavaliados, então um terminal — ou o VS Code — que já
       estava aberto continua sem a permissão nova."
fi
echo -e "Porta: ${YELLOW}$SERIAL_PORT${NC}"

# ====================================================================================
# PASSO 2: PERGUNTAR AO CHIP QUEM ELE É
# ====================================================================================
step "Passo 2: Identificando o chip"

CHIP_RAW=$($ESPTOOL --port "$SERIAL_PORT" --before default-reset --after no-reset flash-id 2>&1 \
           | sed 's/\x1b\[[0-9;]*[A-Za-z]//g')
CHIP_DESC=$(echo "$CHIP_RAW" | grep -m1 'Chip type:' | sed 's/.*Chip type: *//; s/ *$//')
[ -n "$CHIP_DESC" ] || { echo "$CHIP_RAW" >&2; die "não consegui identificar o chip na porta $SERIAL_PORT."; }

# "ESP32-C3 (QFN32) (revision v0.4)" -> "esp32c3"
CHIP=$(echo "$CHIP_DESC" | awk '{print $1}' | tr 'A-Z-' 'a-z ' | tr -d ' ')
FLASH_DETECTED=$(echo "$CHIP_RAW" | grep -m1 'Detected flash size:' | sed 's/.*: *//')

echo -e "Chip:  ${YELLOW}${CHIP_DESC}${NC}"
echo -e "Flash: ${YELLOW}${FLASH_DETECTED:-desconhecida}${NC}"

# O ESP32 e o S2 têm o bootloader em 0x1000; os demais, em 0x0.
case "$CHIP" in
    esp32|esp32s2) BOOTLOADER_ADDR="0x1000" ;;
    *)             BOOTLOADER_ADDR="0x0" ;;
esac

# chip_id gravado no cabeçalho da imagem (esp_image_header_t, offset 12, uint16 LE).
case "$CHIP" in
    esp32)   EXPECTED_ID=0  ;; esp32s2) EXPECTED_ID=2  ;;
    esp32c3) EXPECTED_ID=5  ;; esp32s3) EXPECTED_ID=9  ;;
    esp32c2) EXPECTED_ID=12 ;; esp32c6) EXPECTED_ID=13 ;;
    esp32h2) EXPECTED_ID=16 ;; esp32p4) EXPECTED_ID=18 ;;
    *) die "chip '$CHIP' não é suportado por este script." ;;
esac
echo -e "Bootloader vai em ${YELLOW}${BOOTLOADER_ADDR}${NC} (chip_id esperado: $EXPECTED_ID)"

# ====================================================================================
# PASSO 3: COMPILAR (se não veio um build pronto)
# ====================================================================================
if [ -z "$BUILD_DIR" ]; then
    step "Passo 3: Compilando o firmware"
    command -v arduino-cli >/dev/null || die "arduino-cli não encontrado. Instale, ou passe um diretório de build:
       bash $0 $SERIAL_PORT /caminho/do/build"

    ARDUINO_JSON="${SCRIPT_DIR}/.vscode/arduino.json"
    [ -f "$ARDUINO_JSON" ] || die "não achei $ARDUINO_JSON para saber a placa e as opções de build."
    BOARD=$(python3 -c "import json;print(json.load(open('$ARDUINO_JSON'))['board'])")
    OPTS=$(python3 -c "import json;print(json.load(open('$ARDUINO_JSON')).get('configuration',''))")

    # A placa configurada tem que bater com o chip que está na ponta do cabo.
    BOARD_CHIP=$(echo "$BOARD" | awk -F: '{print $3}')
    case "$BOARD_CHIP" in
        esp32da|esp32|esp32dev|esp32wrover*) BOARD_CHIP="esp32" ;;
    esac
    if [ "$BOARD_CHIP" != "$CHIP" ]; then
        die "a placa conectada é um ${CHIP_DESC%% *}, mas o projeto está configurado para '$BOARD'.
       Ajuste 'board' em .vscode/arduino.json (e revise o mapa de pinos!) antes de gravar."
    fi

    FQBN="$BOARD"; [ -n "$OPTS" ] && FQBN="$BOARD:$OPTS"
    BUILD_DIR=$(mktemp -d -t homeguard-build-XXXXXX)
    CLEAN_BUILD_DIR=1
    echo -e "FQBN: ${BLUE}$FQBN${NC}"
    arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" "$SCRIPT_DIR" \
        || die "a compilação falhou."
else
    step "Passo 3: Usando o build informado"
    [ -d "$BUILD_DIR" ] || die "o diretório de build '$BUILD_DIR' não existe."
    CLEAN_BUILD_DIR=0
    echo -e "Build: ${YELLOW}$BUILD_DIR${NC}"
fi

# ====================================================================================
# PASSO 4: CONFERIR SE O BINÁRIO É DESTE CHIP
# ====================================================================================
step "Passo 4: Conferindo se o binário combina com a placa"

FIRMWARE_BIN=$(find "$BUILD_DIR" -maxdepth 1 -name '*.ino.bin' | head -n 1)
[ -n "$FIRMWARE_BIN" ] || die "não encontrei o firmware (*.ino.bin) em $BUILD_DIR"
PREFIX="${FIRMWARE_BIN%.bin}"
BOOTLOADER_BIN="${PREFIX}.bootloader.bin"
PARTITIONS_BIN="${PREFIX}.partitions.bin"
[ -f "$BOOTLOADER_BIN" ] || die "faltando $BOOTLOADER_BIN"
[ -f "$PARTITIONS_BIN" ] || die "faltando $PARTITIONS_BIN"

IMAGE_ID=$(python3 - "$FIRMWARE_BIN" <<'PY'
import sys
with open(sys.argv[1], 'rb') as fh:
    hdr = fh.read(16)
print(-1 if len(hdr) < 14 or hdr[0] != 0xE9 else int.from_bytes(hdr[12:14], 'little'))
PY
)
if [ "$IMAGE_ID" != "$EXPECTED_ID" ]; then
    die "o firmware foi compilado para outro chip (chip_id=$IMAGE_ID, esperado $EXPECTED_ID).
       Gravar isso deixaria a placa sem bootar. Recompile para ${CHIP_DESC%% *}."
fi
echo -e "${GREEN}OK:${NC} $(basename "$FIRMWARE_BIN") é de fato uma imagem para ${CHIP_DESC%% *}."

BOOT_APP0=""
if grep -qiE '^\s*otadata|,\s*ota\s*,' "${BUILD_DIR}/partitions.csv" 2>/dev/null; then
    BOOT_APP0=$(find "${ARDUINO15_DIR:-$HOME/.arduino15}/packages/esp32" -type f -name boot_app0.bin | head -n 1)
fi

# ====================================================================================
# PASSO 5: INTERFACE WEB + IMAGEM LittleFS
# ====================================================================================
step "Passo 5: Construindo a interface web"

DATA_DIR="${SCRIPT_DIR}/data"
[ -f "${DATA_DIR}/package.json" ] || die "não achei ${DATA_DIR}/package.json"

pushd "$DATA_DIR" > /dev/null
[ -d node_modules ] || { echo "Instalando dependências (npm install)..."; npm install || { popd >/dev/null; die "npm install falhou."; }; }
npm run build || { popd > /dev/null; die "o build da interface web falhou."; }
popd > /dev/null
[ -d "${DATA_DIR}/dist" ] || die "o build não gerou a pasta 'dist'."

step "Passo 6: Montando a imagem LittleFS"

PARTITIONS_CSV="${BUILD_DIR}/partitions.csv"
[ -f "$PARTITIONS_CSV" ] || die "não achei partitions.csv em $BUILD_DIR"
FS_LINE=$(grep -iE 'spiffs|littlefs' "$PARTITIONS_CSV" | head -n 1 | tr -d ' ')
[ -n "$FS_LINE" ] || die "a tabela de partições não tem uma partição de dados (spiffs/littlefs)."
FS_OFFSET=$(echo "$FS_LINE" | cut -d, -f4)
FS_SIZE=$(echo "$FS_LINE" | cut -d, -f5)
[ -n "$FS_OFFSET" ] && [ -n "$FS_SIZE" ] || die "não consegui ler o offset/tamanho da partição de dados."
echo -e "Partição de dados: offset ${YELLOW}${FS_OFFSET}${NC}, tamanho ${YELLOW}${FS_SIZE}${NC}"

WORK_DIR=$(mktemp -d -t homeguard-fs-XXXXXX)
LITTLEFS_IMG="${WORK_DIR}.bin"
cleanup() {
    rm -rf "$WORK_DIR"
    rm -f "$LITTLEFS_IMG"
    [ "${CLEAN_BUILD_DIR:-0}" = "1" ] && rm -rf "$BUILD_DIR"
    return 0
}
trap cleanup EXIT

cp -r "${DATA_DIR}/dist/." "${WORK_DIR}/" || die "falha ao copiar os arquivos da interface."
"$MKLITTLEFS" -c "$WORK_DIR" -b 4096 -p 256 -s "$FS_SIZE" "$LITTLEFS_IMG" \
    || die "falha ao criar a imagem LittleFS."
echo -e "${GREEN}Imagem LittleFS criada.${NC}"

# ====================================================================================
# PASSO 7: GRAVAR
# ====================================================================================
step "Passo 7: Gravando na placa"

WRITE_ARGS=( "$BOOTLOADER_ADDR" "$BOOTLOADER_BIN" 0x8000 "$PARTITIONS_BIN" )
[ -n "$BOOT_APP0" ] && WRITE_ARGS+=( 0xe000 "$BOOT_APP0" )
WRITE_ARGS+=( 0x10000 "$FIRMWARE_BIN" "$FS_OFFSET" "$LITTLEFS_IMG" )

# --flash-mode/freq/size 'keep' preserva o que o próprio build gravou no cabeçalho
# da imagem, em vez de chutar valores fixos que podem não bater com a placa.
$ESPTOOL --chip "$CHIP" --port "$SERIAL_PORT" --baud 921600 \
    --before default-reset --after hard-reset write-flash -z \
    --flash-mode keep --flash-freq keep --flash-size keep \
    "${WRITE_ARGS[@]}" || { rm -f "$LITTLEFS_IMG"; die "a gravação falhou. Confira o cabo e a porta."; }

rm -f "$LITTLEFS_IMG"
echo -e "\n${GREEN}>>> Concluído: firmware + interface web gravados no ${CHIP_DESC%% *}. <<<${NC}"
echo -e "Para acompanhar o boot:  ${BLUE}arduino-cli monitor -p $SERIAL_PORT -c baudrate=115200${NC}"
exit 0
