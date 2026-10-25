# homeguard-admin

Administração do alarme ESP32 Homeguard pelo terminal (TUI), para Linux, macOS e
Windows. Faz tudo o que a página de admin faz, e ainda grava a placa pelo cabo USB.

```
 Homeguard Admin  ● 10.55.9.17   admin ✓                 USB ● ESP32 conectado (/dev/ttyACM0)
 1 Painel │ 2 Zonas │ 3 Senhas │ 4 Tempos │ 5 Sirene │ 6 Rede │ 7 Eventos │ 8 Sistema │ 9 USB
```

## O que tem

- **Busca as placas na rede** sozinho: mDNS (`homeguard-XXXX.local`) e varredura das
  redes /24 do PC, batendo em `/api/info`. Acha a placa mesmo em outra sub-rede
  roteada, onde o mDNS não chega; dá para mandar varrer outra rede (ex.: `10.55.9.0/24`).
- **Painel**: estado, zonas, contagens de entrada/saída; armar (Ausente / Em casa),
  desarmar com senha, silenciar a sirene.
- **Admin** (senha mestre): zonas (nome, tipo, Em casa, inibida), senhas e senha de
  pânico, tempos de entrada/saída, aviso de zona aberta, arme automático, sirene,
  testes de sirene e trava, Telegram, rede (com a busca de redes Wi-Fi pela placa),
  nome/ID MQTT, senha mestre, registro de eventos, backup e restauração.
- **Atualização pela rede (OTA)** com o `homeguard.ino.bin`. Se a versão nova não subir,
  a placa volta sozinha para a anterior.
- **Firmware direto do GitHub**: ao abrir, consulta o último release de
  `duarte-gui/homeguard` (ou o de `HOMEGUARD_REPO`). Na aba Sistema, `d` baixa e já
  oferece o OTA; na aba Gravar USB, `d` baixa e deixa pronto para gravar pelo cabo.
  Os arquivos ficam em `~/.cache/homeguard-admin/`.
- **Gravação pelo cabo USB**, sem Python nem esptool. Bootloader, partições e
  `boot_app0` vêm dentro do programa (`assets/`); só o `homeguard.ino.bin` é escolhido —
  o mesmo arquivo do OTA. A configuração e as senhas ficam, a não ser que se marque
  "apagar tudo". Mostra quando o ESP32 é plugado ou tirado do USB (com o MAC).

## Uso

```bash
homeguard-admin                                # procura as placas na rede
homeguard-admin 192.168.1.18                   # conecta direto
homeguard-admin --usb                          # abre na gravação pelo cabo
homeguard-admin --scan [10.55.9.0/24]          # só lista as placas (sem interface)
homeguard-admin --download                     # baixa o firmware do último release
homeguard-admin --flash homeguard.ino.bin [porta]  # grava pelo cabo (sem interface)
```

Teclas: `1`–`9` trocam de aba, `c` conecta em outra placa, `L` entra no admin, `q` sai.
A linha de baixo mostra as teclas de cada aba.

No Linux, para gravar pelo USB o usuário precisa estar no grupo dono da porta
(`dialout` no Debian/Ubuntu/Fedora, `uucp` no Arch).

## Compilar

```bash
cargo build --release      # target/release/homeguard-admin
```

Os arquivos de `assets/` saem do build do firmware (`homeguard.ino.bootloader.bin`,
`homeguard.ino.partitions.bin` e `boot_app0.bin`, com `PartitionScheme=min_spiffs`). Se
a tabela de partições do firmware mudar, copie os novos para cá e recompile.

Teste de tela contra uma placa de verdade (desenha num buffer, sem terminal):

```bash
HG_TEST_HOST=192.168.1.18 cargo test -- --ignored --nocapture
```
