# ESP32 Homeguard

Sistema de alarme residencial DIY baseado em **ESP32**, com interface web em React/TypeScript e integração com **Home Assistant** via MQTT.

---

## Pré-requisitos (tenha em mãos antes de configurar)

A configuração é feita pelo **celular** (portal hotspot). Antes, junte estes dados — abaixo, como obter cada um:

### 1. Rede Wi-Fi
- **Nome (SSID)** e **senha** da rede **2,4 GHz** onde o ESP32 vai ficar.
  > O ESP32 não conecta em redes 5 GHz.

### 2. Broker MQTT (no Home Assistant)
O ESP fala com o HA por MQTT. Você precisa do **IP do broker**, um **usuário** e uma **senha**:
1. No HA: **Configurações → Complementos (Add-ons) → Loja** → instale **"Mosquitto broker"** → **Iniciar**.
2. Crie um usuário para o ESP: **Configurações → Pessoas → aba _Usuários_ → Adicionar usuário** (ex.: nome `mqtt`, com uma senha). O Mosquitto aceita usuários do próprio HA.
3. Anote:
   - **Broker (IP):** o IP do seu Home Assistant (ex.: `192.168.1.90`).
   - **Porta:** `1883`.
   - **Usuário / Senha:** os criados acima.

### 3. Senha mestre (você escolhe)
Um código de **4 a 8 dígitos**. É a senha mais importante: além de armar/desarmar, é a **única** que pode **programar o teclado** (cadastrar/remover senhas e resetar) e é usada para **desarmar pelo Home Assistant**. Guarde com cuidado.

### 4. Telegram (opcional — alertas de violação)
1. **Bot token:** no Telegram, fale com **@BotFather** → `/newbot` → escolha nome e usuário do bot → ele devolve um **token** (ex.: `123456789:AAH...`). Copie.
2. **Chat ID:** fale com **@userinfobot** (ou **@RawDataBot**) → ele responde com seu **Id** numérico. Copie.
   > Para receber as mensagens, abra uma conversa com o **seu** bot e envie um "oi" pelo menos uma vez.

> Com isso em mãos: ligue o ESP32, conecte o celular ao Wi-Fi **`Homeguard-XXXX`** e preencha o portal.

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
| ESP32 (qualquer variante com 4 MB de flash) | Testado com ESP32-WROOM-32 e ESP32-C3 Super Mini |
| 5× sensor magnético (reed switch) | Um por setor |
| 1× teclado Wiegand (D0/D1) | Para armar/desarmar fisicamente |
| 1× módulo relé (sirene) | |
| 1× módulo relé (trava/portão) | Opcional |
| 1× LED de status | |

### Mapeamento de Pinos

O firmware detecta a placa em tempo de compilação (`CONFIG_IDF_TARGET_ESP32C3`, definido
automaticamente pelo core ao selecionar uma placa C3) e usa o mapa de pinos correspondente —
não precisa editar nada no código, só escolher a placa certa em **Tools → Board**.

**ESP32-WROOM-32** (`esp32:esp32:esp32`):

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

**ESP32-C3 Super Mini** (`esp32:esp32:esp32c3`, placa "ESP32C3 Dev Module"):

| GPIO | Função |
|---|---|
| 0 | Setor 1 — Portão Garagem |
| 1 | Setor 2 — Presença Garagem |
| 3 | Setor 3 — Portão Principal |
| 4 | Setor 4 — Porta Gui |
| 5 | Setor 5 — Porta Gu |
| 6 | Relé Sirene |
| 7 | LED de Status |
| 10 | Relé Trava/Portão |
| 20 | Wiegand D0 |
| 21 | Wiegand D1 |

> O C3 Super Mini só expõe GPIO 0-10 e 20-21 (11-19 são flash/USB interno). Os pinos 2, 8 e 9
> são *strapping pins* de boot e foram evitados de propósito **como entrada** — um sensor que
> fique em nível baixo ("Fechado") ao ligar o ESP nesses pinos pode impedir o boot normal.
> (O GPIO 8 é usado como *saída* pelo LED RGB embutido — ver abaixo; isso é seguro porque
> strapping só importa como entrada, no instante do boot/reset.)
>
> Nessa placa, selecione **Tools → USB CDC On Boot → Enabled** (ela não tem chip USB-serial
> dedicado; é o que libera o GPIO 20/21, normalmente UART0, para uso como GPIO comum).

> **Flash mode:** selecione **Tools → Flash Mode → DIO**, não QIO (o padrão da placa). Muitos
> C3 Super Mini vêm com flash embutida da XMC, que é instável em modo QIO — o sintoma é o board
> resetar sozinho em loop logo depois do boot (`rst:0x7 (TG0WDT_SYS_RST)` no monitor serial).
> `flash.sh` já grava em DIO por padrão.

### LED RGB de status (ESP32-C3 Super Mini)

O LED endereçável embutido (GPIO8, `LED_BUILTIN`) mostra o estado da conexão sem precisar
abrir o monitor serial:

| Cor / padrão | Estado |
|---|---|
| 🔵 Azul piscando rápido | Não configurado — portal hotspot aberto, aguardando configuração |
| 🟡 Amarelo piscando | Configurado, conectando ao Wi-Fi |
| 🟣 Roxo pisca-duplo | Wi-Fi conectado, mas MQTT/Home Assistant fora do ar |
| 🟢 Verde "respirando" | Tudo conectado — sistema pronto |
| 🔴 Vermelho piscando rápido | Alarme disparado (`triggered`) |

Em placas sem LED embutido (ex.: ESP32-WROOM-32 "cru", sem `LED_BUILTIN` definido) essa
sinalização é automaticamente desativada — não precisa editar nada no código.

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
3. Escolha a rede Wi-Fi, informe o **broker MQTT** (IP do Home Assistant), usuário/senha
   e a **senha mestre** (4-8 dígitos). Telegram é opcional.
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

Há uma **senha mestre** (definida no portal) e várias senhas comuns (até 16, em NVS).
A `*` **sempre limpa os dígitos** (use no início de cada senha) — essa função não muda.

**Operar** (qualquer senha comum **ou** a mestre):

| Sequência | Ação |
|---|---|
| `*` `<senha>` `#` | Arma / desarma |

**Programar** (somente a **senha mestre** tem autoridade):

| Sequência | Ação |
|---|---|
| `#` (com o teclado "limpo") | Entra na programação → pede a mestre |
| `<mestre>` `#` | Abre o **menu** |
| menu → `1` → `<nova senha>` `#` | **Cadastra** uma senha |
| menu → `2` → `<senha>` `#` | **Remove** uma senha |
| menu → `0` → `<mestre>` `#` | **Reset de fábrica** (volta ao portal de config) |
| `*` (com dígitos) | Limpa o que foi digitado |
| `*` (já limpo) | Cancela a programação |

> A entrada de programação é o **`#` sem nada digitado** — algo que não acontece ao operar
> (você sempre digita a senha antes do `#`). Assim o `*` segue só limpando, como antes.

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
