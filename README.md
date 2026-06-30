# ESP-NOW Master–Slave

Projeto Arduino para **ESP32** que implementa comunicação **ESP-NOW** no modelo mestre/escravo com:

- **Discovery automático** — o master faz broadcast e os slaves respondem
- **Ping / Pong** — heartbeat a cada 5 s; slave/master marcado como *offline* após 15 s sem resposta
- **Reconexão automática** — master volta a fazer discovery quando detecta slaves offline; slave aguarda novo discovery ao perder contato com o master
- **Comandos via Serial** — envie comandos individuais ou em broadcast pelo monitor serial
- **Nomenclatura única por chip** — cada ESP recebe um nome derivado do ID de fábrica (`SMTX-XXXXXXXXXXXX`)

---

## Estrutura do Projeto

```
esp-now-master-slave/
├── esp_now_master/
│   └── esp_now_master.ino   # Código do Master
├── esp_now_slave/
│   └── esp_now_slave.ino    # Código do Slave
└── README.md
```

---

## Requisitos

| Item | Versão mínima |
|------|--------------|
| Arduino IDE | 2.x |
| Placa: **ESP32** (qualquer variante) | – |
| Suporte ESP32 no Arduino (Board Manager) | 2.0.0+ |

> **Nenhuma biblioteca externa** é necessária — tudo usa a API `esp_now.h` nativa do SDK do ESP32.

---

## Como Gravar

### 1 · Configurar o Board Manager

No Arduino IDE, adicione a URL do ESP32 em **File → Preferences → Additional boards manager URLs**:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

Em seguida, instale o pacote **esp32 by Espressif Systems** via **Tools → Board → Boards Manager**.

### 2 · Gravar o Master

1. Abra `esp_now_master/esp_now_master.ino` no Arduino IDE
2. Selecione a placa correta em **Tools → Board → ESP32 Arduino**
3. Selecione a porta COM correta em **Tools → Port**
4. Clique em **Upload**

### 3 · Gravar o(s) Slave(s)

1. Abra `esp_now_slave/esp_now_slave.ino`
2. Selecione a placa e porta do slave
3. Clique em **Upload**

Repita o passo 3 para cada slave adicional.

---

## Protocolo de Mensagens

Toda comunicação usa uma `struct` de tamanho fixo enviada via ESP-NOW:

```c
typedef struct {
    char type[12];     // tipo da mensagem
    char sender[32];   // nome do remetente (ex: "SMTX-AABBCCDDEEFF")
    char payload[200]; // conteúdo (comando, resposta, etc.)
} Message;
```

| `type`     | Quem envia | Descrição |
|------------|-----------|-----------|
| `DISC`     | Master    | Broadcast de discovery |
| `DISC_ACK` | Slave     | Confirmação de presença ao master |
| `PING`     | Master    | Heartbeat a cada 5 s |
| `PONG`     | Slave     | Resposta ao PING |
| `CMD`      | Master    | Comando para o slave (payload: `#cmd`) |
| `DATA`     | Slave     | Dado/resposta para o master (payload: `@slave_name#cmd`) |

---

## Nomeação dos ESPs

Cada dispositivo recebe um nome único gerado em tempo de execução:

```cpp
String get_esp_name() {
    uint64_t chipid = ESP.getEfuseMac();
    char id_str[13];
    sprintf(id_str, "%012llX", chipid);
    return "SMTX-" + String(id_str);
}
// Exemplo: "SMTX-A1B2C3D4E5F6"
```

O nome é exibido no Serial logo após o boot.

---

## Comandos via Serial — Master

Abra o **Serial Monitor** (115200 baud) conectado ao master.

### Enviar comando para um slave específico

```
@SMTX-A1B2C3D4E5F6#get_info
```

O master extrai o nome `SMTX-A1B2C3D4E5F6` e envia `#get_info` ao slave correspondente.

### Enviar comando para todos os slaves online

```
@all#restart
```

### Listar slaves conhecidos

```
list
```

Saída de exemplo:

```
[MASTER] Slaves conhecidos: 2
  [0] SMTX-A1B2C3D4E5F6  MAC: AA:BB:CC:DD:EE:FF  ONLINE
  [1] SMTX-112233445566   MAC: 11:22:33:44:55:66  OFFLINE
```

---

## Comandos Built-in dos Slaves

Os slaves reconhecem os seguintes comandos (enviados pelo master via `@nome#cmd`):

| Comando | Resposta do Slave |
|---------|------------------|
| `#ping` | `#pong` |
| `#get_info` | `#info:heap=<bytes>,name=<nome>` |
| `#restart` | Reinicia o ESP |
| qualquer outro | `#ack:<comando>` |

---

## Comandos via Serial — Slave

Abra o Serial Monitor (115200 baud) conectado a um slave.

```
#hello
```

O slave envia `@SMTX-XXXX#hello` ao master, que imprime:

```
[SLAVE→MASTER] SMTX-XXXX: @SMTX-XXXX#hello
```

---

## Fluxo de Operação

```
MASTER                                  SLAVE(s)
  │                                        │
  │── DISC (broadcast) ──────────────────►│
  │                                        │── registra master
  │◄─ DISC_ACK ────────────────────────── │
  │── registra slave                       │
  │                                        │
  │  (a cada 5 s)                          │
  │── PING ──────────────────────────────►│
  │◄─ PONG ────────────────────────────── │
  │                                        │
  │  (Serial: @slave#get_info)             │
  │── CMD (#get_info) ───────────────────►│
  │◄─ DATA (@slave#info:...) ─────────── │
  │ print: [SLAVE→MASTER] ...             │
  │                                        │
  │  (15 s sem PONG)                       │
  │── marca slave OFFLINE                  │
  │── DISC (broadcast) ──────────────────►│  ◄ re-discovery automático
```

---

## Configurações Ajustáveis

### Master (`esp_now_master.ino`)

```cpp
#define MAX_SLAVES              10    // máximo de slaves suportados
#define PING_INTERVAL_MS      5000    // intervalo de ping (ms)
#define OFFLINE_TIMEOUT_MS   15000    // timeout para marcar slave offline (ms)
#define DISCOVERY_INTERVAL_MS 10000   // intervalo de re-discovery (ms)
```

### Slave (`esp_now_slave.ino`)

```cpp
#define OFFLINE_TIMEOUT_MS   15000    // timeout para marcar master offline (ms)
```

---

## Exemplo de Saída Serial

### Master

```
=== ESP-NOW MASTER ===
Nome : SMTX-A1B2C3D4E5F6
MAC  : AA:BB:CC:DD:EE:FF
[MASTER] Broadcasting #discovery...
Comandos: @all#cmd | @nome#cmd | list
======================

[MASTER] Novo slave: SMTX-112233445566  MAC: 11:22:33:44:55:66
[MASTER] Slave ONLINE: SMTX-112233445566
[SLAVE→MASTER] SMTX-112233445566: @SMTX-112233445566#hello
[MASTER] Slave OFFLINE: SMTX-112233445566
[MASTER] Broadcasting #discovery...
[MASTER] Slave voltou ONLINE: SMTX-112233445566
```

### Slave

```
=== ESP-NOW SLAVE ===
Nome: SMTX-112233445566
MAC : 11:22:33:44:55:66
Aguardando discovery do master...
Serial: #cmd  (ex: #hello)
====================

[SLAVE] Master detectado: SMTX-A1B2C3D4E5F6
[SLAVE] Comando recebido: #get_info
[SLAVE→MASTER] @SMTX-112233445566#info:heap=234000,name=SMTX-112233445566
[SLAVE] Master OFFLINE. Aguardando re-discovery...
[SLAVE] Master detectado: SMTX-A1B2C3D4E5F6
```

---

## Limitações Conhecidas

- O ESP-NOW opera no canal Wi-Fi atual. Se o Wi-Fi estiver em uso, certifique-se de que todos os dispositivos estejam no mesmo canal.
- Máximo de 20 peers simultâneos (limitação da ESP-NOW API). O projeto suporta até 10 slaves (`MAX_SLAVES`).
- Os nomes são derivados do MAC e são únicos por dispositivo, mas dependem do eFuse gravado pela Espressif.

---

## Licença

MIT — veja [LICENSE](LICENSE).
