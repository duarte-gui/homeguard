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

- Monitoramento de **4 setores físicos** (sensores magnéticos de porta/janela)
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
| ESP32-C3 com 4 MB de flash | Testado com ESP32-C3 QFN32 rev v0.4, flash embutida |
| 4× sensor magnético (reed switch) | Um por setor |
| 1× teclado Wiegand (D0/D1) | Para armar/desarmar fisicamente |
| 1× módulo relé (sirene) | |
| 1× módulo relé (trava/portão) | Opcional |
| 1× LED de status | |

### Mapeamento de Pinos

| GPIO | Função | Observação |
|---|---|---|
| 0 | Setor 1 — Portão Garagem | |
| 1 | Setor 2 — Presença Garagem | |
| 3 | Setor 3 — Portão Principal | |
| 4 | Setor 4 — Porta Gui | |
| 20 | *(livre)* | Era o Setor 5, removido |
| 5 | Relé Trava/Portão | |
| 10 | Relé Sirene | |
| 21 | LED de Status (externo) | UART0 TX — pisca durante o log de boot da ROM |
| 8 | LED RGB embutido | Já vem soldado na devkit; strapping, mas não atrapalha |
| 6 | Wiegand D0 | |
| 7 | Wiegand D1 | |

> Os sensores usam `INPUT_PULLUP`. Estado **Aberto** = HIGH, **Fechado** = LOW.

**Por que este mapa e não outro.** O ESP32-C3 QFN32 com flash embutida tem pouquíssimo
GPIO livre, e o projeto precisa de exatamente 10:

| GPIO | Situação |
|---|---|
| 11 | `VDD_SPI` — inutilizável |
| 12 a 17 | Flash SPI interna — inutilizáveis |
| 18, 19 | USB D− / D+ do USB-Serial-JTAG — é por onde se grava e sai o Serial |
| 2, 8, 9 | Strapping: precisam estar em nível ALTO no boot (o 8 é o LED RGB da placa) |
| 20, 21 | UART0 RX/TX — livres porque o Serial vai pelo USB (`CDCOnBoot=cdc`) |

Nenhum setor pode ficar em 2, 8 ou 9: com `INPUT_PULLUP`, um setor **fechado** puxa o
pino para GND, e isso travaria o boot do chip.

### LED RGB embutido

O LED externo do GPIO 21 continua fazendo só armado/desarmado. O RGB da placa (GPIO 8,
via `rgbLedWrite(RGB_BUILTIN, …)`) mostra os estados que antes só apareciam no Serial:

| Cor | Significado |
|---|---|
| Verde fraco | Desarmado, tudo certo |
| Vermelho fixo | Armado |
| Vermelho piscando | **Alarme disparado** |
| Laranja piscando | Sem Wi-Fi |
| Azul piscando | Portal de configuração aberto |

O brilho é baixo de propósito — um WS2812 no máximo é ofuscante de perto. O código está
dentro de `#ifdef RGB_BUILTIN`, então continua compilando em placas sem LED RGB.

> Cuidado se sua placa for uma **ESP32-C3 SuperMini** ou clone parecido: várias delas
> trazem um LED simples (azul, ligado em nível baixo) no GPIO 8, e não um WS2812. Nesse
> caso o `rgbLedWrite()` gera pulsos que o LED não entende, e ele só pisca sem sentido.

> **Atenção aos relés.** Os GPIOs ficam flutuando durante os ~200 ms de boot, antes do
> `pinMode()` rodar. Use módulos de relé cujo estado de repouso seja o desligado (ou um
> resistor de pull-down na entrada), senão a trava do portão pode dar um pulso a cada reset.

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
reset) — o portal volta a aparecer — há três caminhos:

- **Botão BOOT:** com a placa já ligada, segure o BOOT por **10 s**. O LED RGB pisca vermelho
  durante a contagem (acelera perto do fim) e fica azul quando apaga. Soltar antes cancela.
  Não segure o BOOT *ao ligar* a placa: isso põe o ESP32-C3 em modo de gravação.
- **Teclado:** `#` → `<mestre> #` → `0` → `<mestre> #`.
- **MQTT:** publique `factory_reset` em `homeguard/system/command`.

O reset apaga **tudo** da NVS: Wi-Fi, MQTT, Telegram, senha mestre e as senhas do teclado.

Os tópicos de comando nunca devem ser publicados com *retain*. O firmware apaga qualquer
mensagem retida neles antes de se inscrever — sem isso, um `ON` retido na sirene a disparava
em todo boot, e um `factory_reset` retido apagaria a configuração a cada reinício.

---

## Compilação — Arduino IDE / VS Code (Arduino Extension)

### Pré-requisitos

#### 1. Plataforma ESP32

No Arduino IDE ou VS Code com a extensão **Arduino** instalada:

- Adicione a URL de placas ESP32 em **File → Preferences → Additional Boards Manager URLs**:
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```
- Abra **Tools → Board → Boards Manager**, pesquise `esp32` e instale o pacote da **Espressif Systems** — série **3.x** (testado com 3.3.11).

> A série 2.x não é mais publicada no índice da Espressif. O firmware usa a API do core 3.x.

#### 2. Bibliotecas Arduino (instalar pelo Library Manager)

| Biblioteca | Autor | Versão testada |
|---|---|---|
| **ESP Async WebServer** | ESP32Async | 3.12.0 |
| **Async TCP** | ESP32Async | 3.5.0 |
| **ArduinoJson** | Benoit Blanchon | 7.4.3 |
| **UniversalTelegramBot** | Brian Lough | 1.3.0 |
| **PubSubClient** | Nick O'Leary | 2.8 |

> Use o fork **ESP32Async**, não o original do Me-No-Dev — este último não compila no core 3.x. Ambas estão no Library Manager oficial; o header continua sendo `ESPAsyncWebServer.h`, sem mudança no código.

> **LittleFS não precisa de biblioteca.** Vem embutido no core 3.x — a antiga `LittleFS_esp32` (lorol) não é mais necessária.

#### 2b. Instalação por linha de comando (alternativa)

```bash
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "ESP Async WebServer" "Async TCP" ArduinoJson PubSubClient UniversalTelegramBot
```

#### 3. Wiegand (local — não instalar)

Os arquivos `Wiegand.h` e `Wiegand.cpp` já estão na pasta do sketch. **Não é necessário instalar nenhuma biblioteca Wiegand externa.**

#### 4. Configuração da placa no VS Code

No arquivo `.vscode/arduino.json` (criado pela extensão Arduino):

```json
{
  "board": "esp32:esp32:esp32c3",
  "configuration": "JTAGAdapter=default,CDCOnBoot=cdc,PartitionScheme=huge_app,CPUFreq=160,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,DebugLevel=none,EraseFlash=none,ZigbeeMode=default",
  "port": "/dev/ttyACM0",
  "programmer": "esp32:esptool",
  "sketch": "homeguard.ino"
}
```

> Ajuste `"port"` para a porta serial correta. O C3 usa USB nativo e aparece como
> `/dev/ttyACM*` no Linux (`COM*` no Windows) — **não** `/dev/ttyUSB*`, que é de placas
> com conversor CH340/CP210x. O caminho em `/dev/serial/by-id/` é estável e não muda
> conforme a ordem de plugagem.

> **`CDCOnBoot=cdc` é obrigatório.** Os pinos da UART0 (GPIO 20/21) foram usados como
> GPIO, então o `Serial` precisa sair pelo USB. Com `CDCOnBoot=default` você não veria
> log nenhum.

**Sobre o `PartitionScheme=huge_app`:** no esquema `default` o firmware ocupa 91% da partição de app (1,19 MB de 1,31 MB), sem margem para crescer, porque metade da flash fica reservada para uma partição OTA que o projeto não usa. O `huge_app` (3 MB app / 1 MB SPIFFS) derruba o uso para 37% e ainda deixa ~900 KB de sistema de arquivos para uma interface de 172 KB. O custo é abrir mão de OTA — o deploy é sempre por serial via `flash.sh`.

> No Linux, o usuário precisa estar no grupo dono do device (`uucp` no Arch, `dialout`
> no Debian/Ubuntu): `sudo usermod -aG uucp $USER` — é preciso relogar depois.

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

O script `flash.sh` faz tudo em uma execução: descobre a porta, identifica o chip,
compila, confere se o binário é daquele chip, faz o build da interface, monta a imagem
LittleFS e grava.

### Pré-requisitos do script

- **esptool** e **mklittlefs** — vêm no pacote ESP32 do Arduino; o script **detecta a
  versão instalada sozinho** sob `$HOME/.arduino15`, sem editar caminho nenhum.
- **Node.js e npm** — para o build da interface web.
- **arduino-cli** — só é necessário se você não passar um diretório de build pronto.

Para apontar para uma instalação fora do padrão, exporte antes de rodar:

```bash
export ESPTOOL_DIR="/caminho/para/esptool_py/5.3.1/"        # com barra no final
export MKLITTLEFS_DIR="/caminho/para/mklittlefs/4.0.2-db0513a"
```

### Uso

```bash
bash flash.sh                          # descobre a porta e compila sozinho
bash flash.sh /dev/ttyACM0             # porta fixa, compila sozinho
bash flash.sh /dev/ttyACM0 /tmp/build  # usa um build já pronto (ex.: da IDE)
```

O que ele faz, na ordem:

1. **Descobre a porta** — prefere `/dev/serial/by-id/`, que é estável. Se houver mais de
   uma placa conectada, ele lista e pede que você escolha. Se faltar permissão, ele diz
   o grupo exato e o comando para corrigir.
2. **Identifica o chip** perguntando ao próprio silício via `esptool`, e daí deriva o
   offset do bootloader: **0x1000** no ESP32 e no S2, **0x0** em C3/C6/H2/S3/P4.
3. **Compila** com o FQBN de `.vscode/arduino.json` — e **aborta** se a placa configurada
   ali não for a que está no cabo.
4. **Confere o binário**: lê o `chip_id` do cabeçalho da imagem (`esp_image_header_t`) e
   compara com o chip detectado. Se não bater, não grava — em vez de deixar a placa sem
   bootar.
5. `npm install` + `npm run build` na pasta `data/`.
6. Lê a tabela de partições do build para achar o offset e o tamanho reais do LittleFS.
7. Grava bootloader, tabela de partições, `boot_app0`, firmware e sistema de arquivos,
   com `--flash-mode/freq/size keep` (preserva o que o build gravou no cabeçalho, em vez
   de chutar valores fixos).
8. Limpa os temporários.

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
