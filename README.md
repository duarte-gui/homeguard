# ESP32 Homeguard

Sistema de alarme residencial DIY baseado em **ESP32**, com interface web em React/TypeScript e integração com **Home Assistant** via MQTT.

---

## Funcionalidades

- Monitoramento de **5 setores físicos** (sensores magnéticos de porta/janela)
- Interface web em tempo real via **WebSocket**
- Armar/Desarmar pelo navegador, pelo **Home Assistant** ou pelo **teclado Wiegand**
- **Sirene** acionada automaticamente ao detectar violação com o sistema armado
  - **Auto-corte por timeout** (configurável) e **switch de gating** no HA ("Sirene habilitada") — permite **disparar o alerta sem tocar a sirene física**
- **Notificações via Telegram** com o setor violado
- **Auto-discovery MQTT** — sensores e painel de alarme aparecem automaticamente no Home Assistant
  - Estado **`triggered`** do painel + **availability/LWT** (entidades ficam `unavailable` se o ESP cair)
- **Configuração por hotspot** (captive portal no celular) — sem segredos no código, tudo em NVS
- **Cadastro de senhas** do teclado — local (no próprio teclado) e remoto (MQTT / interface web)
- Arquivos da interface servidos diretamente do ESP32 via **LittleFS** (sem depender de CDN externo)

---

## Hardware

### Componentes

| Componente | Observação |
|---|---|
| ESP32 (qualquer variante com 4 MB de flash) | Testado com ESP32-WROOM-32 |
| 5× sensor magnético (reed switch) | Um por setor |
| 1× teclado Wiegand (D0/D1) | Para armar/desarmar fisicamente |
| 1× módulo relé (sirene) | |
| 1× módulo relé (trava/portão) | Opcional |
| 1× LED de status | |

### Mapeamento de Pinos

| GPIO | Função |
|---|---|
| 13 | Setor 1 — Portão Garagem |
| 12 | Setor 2 — Presença Garagem |
| 14 | Setor 3 — Portão Principal |
| 27 | Setor 4 — Porta Gui |
| 26 | Setor 5 — Porta Gu |
| 4 | Relé Sirene |
| 16 | LED de Status |
| 17 | Relé Trava/Portão |
| 18 | Wiegand D0 |
| 19 | Wiegand D1 |

> Os sensores usam `INPUT_PULLUP`. Estado **Aberto** = HIGH, **Fechado** = LOW.

---

## Estrutura do Projeto

```
homeguard/
├── homeguard.ino          # Firmware principal do ESP32
├── config.h               # struct Config (NVS) + KeypadCode
├── storage.h / .cpp       # Persistência em NVS (config + senhas)
├── provisioning.h / .cpp  # Portal hotspot (captive portal) de configuração
├── codes.h / .cpp         # Gerência das senhas do teclado
├── Wiegand.h              # Biblioteca Wiegand local
├── Wiegand.cpp            # Biblioteca Wiegand local
├── flash.sh               # Script de deploy automatizado
├── data/                  # Interface web (React + TypeScript)
│   ├── index.html
│   ├── index.tsx
│   ├── index.css
│   ├── App.tsx
│   ├── types.ts
│   ├── constants.ts
│   ├── .postcssrc
│   ├── package.json
│   └── components/
│       ├── Header.tsx
│       ├── ControlPanel.tsx
│       ├── SectorStatusCard.tsx
│       └── icons/
```

---

## Configuração — portal hotspot (sem editar código)

> **Não há mais segredos no código.** Wi-Fi, MQTT, Telegram, PIN e a senha inicial
> são preenchidos pelo celular, num portal de configuração (captive portal).

Na **primeira vez** (ou se o Wi-Fi salvo falhar), o ESP sobe um hotspot:

1. No celular, conecte-se à rede **`Homeguard-XXXX`**.
2. A tela de configuração abre sozinha (captive portal) — ou acesse `http://192.168.4.1`.
3. Escolha a rede Wi-Fi, informe o **broker MQTT** (IP do Home Assistant), usuário/senha,
   o **PIN** de desarme e uma **senha inicial** do teclado. Telegram é opcional.
4. Toque em **Salvar e Reiniciar**. O ESP grava tudo em NVS e conecta na sua rede.

A configuração fica persistida (`Preferences`/NVS). Para **reconfigurar do zero** (factory
reset), publique `factory_reset` no tópico MQTT `homeguard/system/command` (ou apague a NVS),
e o portal volta a aparecer.

---

## Compilação — Arduino IDE / VS Code (Arduino Extension)

### Pré-requisitos

#### 1. Plataforma ESP32

No Arduino IDE ou VS Code com a extensão **Arduino** instalada:

- Adicione a URL de placas ESP32 em **File → Preferences → Additional Boards Manager URLs**:
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```
- Abra **Tools → Board → Boards Manager**, pesquise `esp32` e instale o pacote da **Espressif Systems** (versão 2.x recomendada).

#### 2. Bibliotecas Arduino (instalar pelo Library Manager)

| Biblioteca | Autor | Versão mínima |
|---|---|---|
| **ESPAsyncWebServer** | Me-No-Dev | qualquer |
| **AsyncTCP** | Me-No-Dev | qualquer |
| **ArduinoJson** | Benoit Blanchon | 7.x |
| **UniversalTelegramBot** | Brian Lough | qualquer |
| **PubSubClient** | Nick O'Leary | 2.8+ |
| **LittleFS_esp32** | lorol | qualquer |

> **ESPAsyncWebServer** e **AsyncTCP** não estão no Library Manager oficial. Instale via `.zip` ou pelo gerenciador de bibliotecas da IDE (repositório: `https://github.com/me-no-dev/ESPAsyncWebServer`).

#### 3. Wiegand (local — não instalar)

Os arquivos `Wiegand.h` e `Wiegand.cpp` já estão na pasta do sketch. **Não é necessário instalar nenhuma biblioteca Wiegand externa.**

#### 4. Configuração da placa no VS Code

No arquivo `.vscode/arduino.json` (criado pela extensão Arduino):

```json
{
  "board": "esp32:esp32:esp32",
  "configuration": "FlashSize=4MB,PartitionScheme=default,CPUFreq=240,FlashMode=dio,FlashFreq=80,UploadSpeed=921600,DebugLevel=none",
  "port": "/dev/ttyUSB0",
  "sketch": "homeguard.ino"
}
```

> Ajuste `"port"` para a porta serial correta (`/dev/ttyUSB0` no Linux, `COM3` no Windows).

---

## Interface Web — Build

A interface web fica em `data/` e precisa ser compilada antes de gravar no ESP32.

### Pré-requisitos

- **Node.js** 18+ e **npm**

### Comandos

```bash
cd data
npm install
npm run build   # Gera os arquivos em data/dist/
```

O build usa **Parcel** e gera arquivos otimizados com Tailwind CSS bundlado localmente em `data/dist/`.

---

## Deploy no ESP32

O script `flash.sh` automatiza todo o processo: faz o build da interface, cria a imagem LittleFS e grava firmware + sistema de arquivos no ESP32 em uma única execução.

### Pré-requisitos do script

- **esptool** (incluído no pacote ESP32 do Arduino)
- **mklittlefs** (incluído no pacote ESP32 do Arduino)

Os caminhos das ferramentas estão configurados no início de `flash.sh` — ajuste se necessário:

```bash
ESP32_TOOLS_BASE_PATH="/home/usuario/.arduino15/packages/esp32/tools/esptool_py/5.0.0/"
MKLIITLEFS_PATH="/home/usuario/.arduino15/packages/esp32/tools/mklittlefs/3.0.0-gnu12-dc7f933"
```

### Uso

1. Compile o sketch na Arduino IDE/VS Code para gerar os binários (o diretório de build é exibido no output da compilação, ex: `/tmp/arduino_build_123456`).
2. Execute o script na raiz do projeto:

```bash
bash flash.sh /dev/ttyUSB0 /tmp/arduino_build_123456
```

O script executa os seguintes passos automaticamente:
1. `npm install` + `npm run build` na pasta `data/`
2. Lê a tabela de partições para obter o endereço correto do LittleFS
3. Cria a imagem `littlefs.bin` com `mklittlefs`
4. Grava bootloader, firmware e sistema de arquivos via `esptool`
5. Limpa os arquivos temporários

---

## Integração com Home Assistant

Após o ESP32 iniciar e conectar ao broker MQTT, as entidades são criadas automaticamente no Home Assistant via **MQTT Discovery**:

| Entidade | Tipo | Tópico de estado |
|---|---|---|
| ESP32 Homeguard (painel) | `alarm_control_panel` | `homeguard/alarm/state` (`disarmed`/`armed_away`/`triggered`) |
| ESP32 Homeguard Siren | `switch` | `homeguard/siren/state` |
| ESP32 Homeguard Sirene habilitada | `switch` | `homeguard/siren_enabled/state` (gating) |
| Portão Garagem | `binary_sensor` | `homeguard/sector/1/state` |
| Presença Garagem | `binary_sensor` | `homeguard/sector/2/state` |
| Portão Principal | `binary_sensor` | `homeguard/sector/3/state` |
| Porta Gui | `binary_sensor` | `homeguard/sector/4/state` |
| Porta Gu | `binary_sensor` | `homeguard/sector/5/state` |

**Armar** não exige código. **Desarmar** exige o **PIN** definido no portal de configuração.
Quando há violação com o sistema armado, o painel vai para `triggered` (mesmo que a sirene
física esteja desabilitada) — útil para automações no HA (ex.: alerta falado na Alexa).

---

## Teclado físico e cadastro de senhas

As senhas do teclado ficam em NVS (lista, até 16). Protocolo do teclado Wiegand:

| Sequência | Ação |
|---|---|
| `<senha válida>` + `#` | Arma / desarma |
| `<senha válida>` + `*` | Entra no **modo programação** |
| (no modo prog.) `<nova senha>` + `#` | **Cadastra** a nova senha |
| `*` | Cancela / sai do modo programação |

**Cadastro remoto** (sem ir ao teclado):

- **MQTT** — publique no tópico `homeguard/codes/set`:
  ```json
  {"action":"add","code":"4567","label":"Gui"}
  {"action":"remove","code":"4567"}
  ```
- **Interface web (WebSocket)** — comandos `{"command":"addCode","code":"...","label":"..."}`
  e `{"command":"removeCode","code":"..."}`.

> A lista publicada em `homeguard/codes/list` mostra apenas **rótulos e tamanho** — nunca os
> valores das senhas.

---

## Tópicos MQTT (avançado)

| Tópico | Direção | Uso |
|---|---|---|
| `homeguard/availability` | ESP→ | `online` / `offline` (LWT) |
| `homeguard/alarm/command` | →ESP | `ARM_AWAY` / `DISARM` |
| `homeguard/siren/command` | →ESP | `ON` (ativa) / `OFF` (silencia) |
| `homeguard/siren_enabled/command` | →ESP | `ON` / `OFF` (gating da sirene física) |
| `homeguard/codes/set` | →ESP | JSON add/remove de senha |
| `homeguard/system/command` | →ESP | `reboot` / `factory_reset` |

---

## Acesso à Interface Web

Após gravar o ESP32, acesse pelo navegador:

```
http://<IP_DO_ESP32>
```

O IP é exibido no monitor serial após a conexão Wi-Fi.

---

## Licença

Uso pessoal / DIY.
