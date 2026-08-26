#!/bin/bash

# ====================================================================================
# ESP32 Homeguard Flasher - Versão Unificada (Tudo-em-Um)
# ====================================================================================
#
# Este script automatiza TODO o processo de deploy:
# 1. Constrói a interface web (executa 'npm install' e 'npm run build').
# 2. Lê a tabela de partições para obter os endereços corretos.
# 3. Cria a imagem do sistema de arquivos (LittleFS).
# 4. Grava o firmware e a interface web no ESP32.
#
# --- PRÉ-REQUISITOS ---
# - Node.js e npm devem estar instalados.
# - O projeto DEVE ser compilado na IDE Arduino antes para gerar os binários.
#
# --- COMO USAR ---
# 1. Compile o projeto na IDE Arduino.
# 2. Na raiz do projeto, execute o script:
#    bash flash_esp32.txt /dev/ttyUSB0 /tmp/arduino_build_XXXXXX
#
# ====================================================================================

# --- CONFIGURAÇÕES ---
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# --- VALIDAÇÃO DOS ARGUMENTOS ---
if [ "$#" -ne 2 ]; then
    echo -e "${RED}Erro: Uso incorreto.${NC}"
    echo "Por favor, forneça a porta serial e o diretório de compilação."
    echo -e "Exemplo: ${YELLOW}bash $0 /dev/ttyUSB0 /tmp/arduino_build_123456${NC}"
    exit 1
fi

SERIAL_PORT=$1
BUILD_DIR=$2

# --- DETECÇÃO DO DIRETÓRIO DO PROJETO ---
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

echo -e "${GREEN}--- Iniciando o processo de gravação do ESP32 Homeguard ---${NC}"
echo -e "Porta Serial: ${YELLOW}$SERIAL_PORT${NC}"
echo -e "Diretório de Compilação: ${YELLOW}$BUILD_DIR${NC}"
echo -e "Raiz do Projeto detectada: ${YELLOW}$SCRIPT_DIR${NC}"

# --- VERIFICAÇÕES ESSENCIAIS ---
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Erro: O diretório de compilação '$BUILD_DIR' não foi encontrado.${NC}"
    exit 1
fi

# --- PASSO 1: PREPARANDO A INTERFACE WEB (Build Automático) ---
echo -e "\n${GREEN}--- Passo 1: Preparando a Interface Web (Build Automático) ---${NC}"
DATA_DIR="${SCRIPT_DIR}/data"

if [ ! -d "$DATA_DIR" ] || [ ! -f "${DATA_DIR}/package.json" ]; then
    echo -e "${RED}Erro: A pasta 'data' ou o arquivo 'package.json' não foi encontrado.${NC}"
    echo "A estrutura do projeto parece estar incorreta."
    exit 1
fi

# Entra na pasta da interface para executar os comandos npm
pushd "$DATA_DIR" > /dev/null

if [ ! -d "node_modules" ]; then
    echo "Pasta 'node_modules' não encontrada. Executando 'npm install' (isso pode levar um minuto)..."
    npm install
    if [ $? -ne 0 ]; then
        echo -e "${RED}Erro durante 'npm install'. Verifique a instalação do Node.js e do npm.${NC}"
        popd > /dev/null
        exit 1
    fi
fi

echo "Construindo a aplicação web com 'npm run build'..."
npm run build

if [ $? -ne 0 ]; then
    echo -e "${RED}Erro ao construir a aplicação web. Verifique a saída do Parcel.${NC}"
    popd > /dev/null
    exit 1
fi

echo -e "${GREEN}Interface web construída com sucesso!${NC}"

# Retorna ao diretório original
popd > /dev/null

# --- VERIFICAÇÃO PÓS-BUILD ---
DIST_DIR="${SCRIPT_DIR}/data/dist"
if [ ! -d "$DIST_DIR" ]; then
    echo -e "${RED}Erro: A pasta 'dist' não foi criada pelo processo de build!${NC}"
    exit 1
fi


# --- CAMINHOS ESTÁTICOS PARA AS FERRAMENTAS DO ESP32 ---
ESP32_TOOLS_BASE_PATH="/home/tecnologia/.arduino15/packages/esp32/tools/esptool_py/5.0.0/"
MKLIITLEFS_PATH="/home/tecnologia/.arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933"

# Validação dos caminhos das ferramentas
if [ ! -d "$ESP32_TOOLS_BASE_PATH" ]; then
    echo -e "${RED}Erro: O diretório da ferramenta 'esptool_py' não foi encontrado: ${YELLOW}$ESP32_TOOLS_BASE_PATH${NC}"
    exit 1
fi
if [ ! -d "$MKLIITLEFS_PATH" ]; then
    echo -e "${RED}Erro: O caminho para 'mklittlefs' não foi encontrado: ${YELLOW}$MKLIITLEFS_PATH${NC}"
    exit 1
fi

# Detecta se esptool é um binário executável ou um script python
ESPTOOL_CMD=""
if [ -f "${ESP32_TOOLS_BASE_PATH}esptool" ] && [ -x "${ESP32_TOOLS_BASE_PATH}esptool" ]; then
    ESPTOOL_CMD="${ESP32_TOOLS_BASE_PATH}esptool"
    echo -e "Usando executável esptool em: ${YELLOW}$ESPTOOL_CMD${NC}"
elif [ -f "${ESP32_TOOLS_BASE_PATH}esptool.py" ]; then
    ESPTOOL_CMD="python3 ${ESP32_TOOLS_BASE_PATH}esptool.py"
    echo -e "Usando script esptool.py em: ${YELLOW}${ESP32_TOOLS_BASE_PATH}esptool.py${NC}"
else
    echo -e "${RED}Erro: Não foi possível encontrar nem 'esptool' nem 'esptool.py' no diretório de ferramentas.${NC}"
    exit 1
fi
MKLITTLEFS="$MKLIITLEFS_PATH/mklittlefs"
echo -e "mklittlefs sendo usado de: ${YELLOW}$MKLITTLEFS${NC}"

# --- DETECÇÃO DO CHIP (ESP32 clássico x ESP32-C3/S2/S3/...) ---
# O offset do bootloader e o --chip do esptool mudam conforme a família:
# o ESP32 "clássico" (Xtensa) grava o bootloader em 0x1000; os chips mais
# novos baseados em RISC-V/USB nativo (C3, C6, H2, S2, S3) gravam em 0x0.
CHIP="esp32"
BOOTLOADER_OFFSET="0x1000"
BUILD_OPTIONS_JSON="${BUILD_DIR}/build.options.json"
if [ -f "$BUILD_OPTIONS_JSON" ]; then
    FQBN=$(grep -o '"fqbn"[[:space:]]*:[[:space:]]*"[^"]*"' "$BUILD_OPTIONS_JSON" | sed -E 's/.*"fqbn"[[:space:]]*:[[:space:]]*"([^"]*)"/\1/')
    if [ -n "$FQBN" ]; then
        echo -e "FQBN detectado em build.options.json: ${YELLOW}$FQBN${NC}"
        for c in esp32c3 esp32c6 esp32h2 esp32s2 esp32s3; do
            if [[ "$FQBN" == *":$c:"* || "$FQBN" == *":$c" ]]; then
                CHIP="$c"
                BOOTLOADER_OFFSET="0x0"
                break
            fi
        done
    fi
fi
echo -e "Chip alvo: ${YELLOW}$CHIP${NC} (bootloader em ${YELLOW}$BOOTLOADER_OFFSET${NC})"
if [ "$CHIP" != "esp32" ]; then
    echo -e "${YELLOW}Nota:${NC} chips dessa família (embutida XMC é comum no C3 Super Mini) podem"
    echo -e "travar em boot com flash mode QIO. Este script já usa DIO por padrão — se"
    echo -e "mesmo assim o board resetar sozinho (rst:0x7 / TG0WDT_SYS_RST no serial),"
    echo -e "é esse o motivo."
fi


# --- PASSO 2: LER A TABELA DE PARTIÇÕES PARA OBTER ENDEREÇO E TAMANHO ---
echo -e "\n${GREEN}--- Passo 2: Lendo a Tabela de Partições ---${NC}"
PARTITIONS_CSV="${BUILD_DIR}/partitions.csv"
if [ ! -f "$PARTITIONS_CSV" ]; then
    echo -e "${RED}Erro: Arquivo 'partitions.csv' não encontrado em $BUILD_DIR${NC}"
    echo "Certifique-se de que a compilação na IDE Arduino foi concluída."
    exit 1
fi

# Extrai o offset e o tamanho da partição de dados (spiffs ou littlefs)
PARTITION_INFO=$(grep -i -E 'spiffs|littlefs' "$PARTITIONS_CSV" | head -n 1 | awk -F, '{gsub(/ /,""); print $4 "," $5}')
if [ -z "$PARTITION_INFO" ]; then
    echo -e "${RED}Erro: Nenhuma partição 'spiffs' ou 'littlefs' foi encontrada em 'partitions.csv'.${NC}"
    exit 1
fi

LITTLEFS_OFFSET=$(echo $PARTITION_INFO | cut -d, -f1)
LITTLEFS_SIZE=$(echo $PARTITION_INFO | cut -d, -f2)

if [ -z "$LITTLEFS_OFFSET" ] || [ -z "$LITTLEFS_SIZE" ]; then
    echo -e "${RED}Erro ao extrair o Offset ou o Tamanho da partição de 'partitions.csv'.${NC}"
    exit 1
fi
echo -e "Partição de dados detectada em:"
echo -e "  - Endereço (Offset): ${YELLOW}$LITTLEFS_OFFSET${NC}"
echo -e "  - Tamanho (Size):    ${YELLOW}$LITTLEFS_SIZE${NC}"


# --- PASSO 3: CRIAR IMAGEM LittleFS A PARTIR DA PASTA 'dist' ---
echo -e "\n${GREEN}--- Passo 3: Criando a imagem do sistema de arquivos (LittleFS) ---${NC}"

DATA_DIR_TEMP="data_temp"
LITTLEFS_IMG="littlefs.bin"

rm -rf $DATA_DIR_TEMP
rm -f $LITTLEFS_IMG
mkdir $DATA_DIR_TEMP

echo "Copiando arquivos construídos de '$DIST_DIR'..."
cp -r "${DIST_DIR}/." "$DATA_DIR_TEMP/"

if [ $? -ne 0 ]; then
    echo -e "${RED}Erro ao copiar os arquivos da pasta 'dist'.${NC}"
    rm -rf $DATA_DIR_TEMP
    exit 1
fi

echo "Criando a imagem $LITTLEFS_IMG com tamanho $LITTLEFS_SIZE..."
$MKLITTLEFS -c $DATA_DIR_TEMP -b 4096 -p 256 -s $LITTLEFS_SIZE $LITTLEFS_IMG

if [ $? -ne 0 ]; then
    echo -e "${RED}Erro ao criar a imagem LittleFS. O processo foi abortado.${NC}"
    rm -rf $DATA_DIR_TEMP
    exit 1
fi
echo -e "${GREEN}Imagem '$LITTLEFS_IMG' criada com sucesso!${NC}"


# --- PASSO 4: GRAVAR TUDO NO ESP32 ---
echo -e "\n${GREEN}--- Passo 4: Gravando o firmware e o sistema de arquivos no ESP32 ---${NC}"
echo "Isso pode levar alguns instantes. Coloque o ESP32 em modo de boot se necessário."

# --- Detecção dinâmica dos arquivos de firmware ---
echo "Detectando arquivos de firmware no diretório de compilação..."
FIRMWARE_INO_BIN=$(find "$BUILD_DIR" -maxdepth 1 -name "*.ino.bin")
if [ -z "$FIRMWARE_INO_BIN" ]; then
    echo -e "${RED}Erro: Não foi possível encontrar o arquivo principal do firmware (*.ino.bin).${NC}"
    rm -rf $DATA_DIR_TEMP; rm -f $LITTLEFS_IMG; exit 1
fi

FIRMWARE_PREFIX=${FIRMWARE_INO_BIN%.bin}
echo "Prefixo do firmware detectado: ${YELLOW}${FIRMWARE_PREFIX}${NC}"

BOOTLOADER_BIN="${FIRMWARE_PREFIX}.bootloader.bin"
PARTITIONS_BIN="${FIRMWARE_PREFIX}.partitions.bin"
FIRMWARE_BIN="$FIRMWARE_INO_BIN"
ESP32_PACKAGE_PATH=$(dirname $(dirname $(dirname "$ESP32_TOOLS_BASE_PATH")))
BOOT_APP0_BIN=$(find "$ESP32_PACKAGE_PATH" -type f -name "boot_app0.bin" | head -n 1)

# Validação da existência dos arquivos
if [ ! -f "$BOOTLOADER_BIN" ] || [ ! -f "$PARTITIONS_BIN" ] || [ ! -f "$FIRMWARE_BIN" ] || [ ! -f "$BOOT_APP0_BIN" ]; then
    echo -e "${RED}Erro: Um ou mais arquivos de firmware não foram encontrados.${NC}"
    rm -rf $DATA_DIR_TEMP; rm -f $LITTLEFS_IMG; exit 1
fi

# Grava tudo usando o offset dinâmico para a imagem LittleFS
$ESPTOOL_CMD --chip $CHIP --port $SERIAL_PORT --baud 921600 --before default-reset --after hard-reset write-flash \
 -z --flash-mode dio --flash-freq 80m --flash-size 4MB \
 $BOOTLOADER_OFFSET "$BOOTLOADER_BIN" \
 0x8000 "$PARTITIONS_BIN" \
 0xe000 "$BOOT_APP0_BIN" \
 0x10000 "$FIRMWARE_BIN" \
 "$LITTLEFS_OFFSET" "$LITTLEFS_IMG"

if [ $? -ne 0 ]; then
    echo -e "\n${RED}Erro durante a gravação no ESP32. Verifique a conexão e a porta serial.${NC}"
else
    echo -e "\n${GREEN}>>> Processo concluído com sucesso! <<<${NC}"
    echo "O ESP32 foi gravado com o novo firmware e a interface web."
fi


# --- PASSO 5: LIMPEZA ---
echo -e "\n${GREEN}--- Passo 5: Limpando arquivos temporários ---${NC}"
rm -rf $DATA_DIR_TEMP
rm -f $LITTLEFS_IMG
echo "Limpeza concluída."

exit 0