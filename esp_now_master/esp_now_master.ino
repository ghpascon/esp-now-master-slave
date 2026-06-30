/*
 * ESP-NOW Master
 * ──────────────
 * Responsabilidades:
 *  - Faz discovery (broadcast) para encontrar slaves
 *  - Mantém lista de peers com status online/offline
 *  - Envia #ping a cada PING_INTERVAL_MS; marca offline após OFFLINE_TIMEOUT_MS
 *  - Aceita comandos via Serial: @all#cmd  ou  @nome_slave#cmd
 *  - Imprime mensagens recebidas dos slaves (@nome#cmd)
 *  - Re-dispara discovery quando há slaves offline
 *
 * Protocolo (struct Message):
 *   type    – tipo da mensagem (DISC, DISC_ACK, PING, PONG, CMD, DATA)
 *   sender  – nome ESP do remetente
 *   payload – conteúdo (comando, resposta, etc.)
 */

#include <esp_now.h>
#include <WiFi.h>

// ─── Configurações ───────────────────────────────────────────────────────────
#define MAX_SLAVES              10
#define PING_INTERVAL_MS      5000   // intervalo de ping
#define OFFLINE_TIMEOUT_MS   15000   // tempo sem resposta → offline
#define DISCOVERY_INTERVAL_MS 10000  // rebroadcast de discovery

// ─── Tipos de Mensagem ───────────────────────────────────────────────────────
#define MSG_DISC      "DISC"
#define MSG_DISC_ACK  "DISC_ACK"
#define MSG_PING      "PING"
#define MSG_PONG      "PONG"
#define MSG_CMD       "CMD"
#define MSG_DATA      "DATA"

// ─── Estrutura de Mensagem ───────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    char type[12];
    char sender[32];
    char payload[200];
} Message;

// ─── Registro de Slave ───────────────────────────────────────────────────────
typedef struct {
    char    name[32];
    uint8_t mac[6];
    bool    online;
    uint32_t last_seen_ms;
} SlaveInfo;

// ─── Globais ─────────────────────────────────────────────────────────────────
String    master_name;
SlaveInfo slaves[MAX_SLAVES];
int       slave_count       = 0;
uint32_t  last_ping_ms      = 0;
uint32_t  last_discovery_ms = 0;

uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── Utilitários ─────────────────────────────────────────────────────────────
String get_esp_name() {
    uint64_t chipid = ESP.getEfuseMac();
    char id_str[13];
    sprintf(id_str, "%012llX", chipid);
    return "SMTX-" + String(id_str);
}

void mac_to_str(const uint8_t *mac, char *buf, size_t buf_len) {
    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int find_slave_by_mac(const uint8_t *mac) {
    for (int i = 0; i < slave_count; i++) {
        if (memcmp(slaves[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

int find_slave_by_name(const char *name) {
    for (int i = 0; i < slave_count; i++) {
        if (strcmp(slaves[i].name, name) == 0) return i;
    }
    return -1;
}

// ─── Gerenciamento de Peers ──────────────────────────────────────────────────
void add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// ─── Envio de Mensagem ───────────────────────────────────────────────────────
void send_message(const uint8_t *mac, const char *type, const char *payload) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.type,    type,              sizeof(msg.type)    - 1);
    strncpy(msg.sender,  master_name.c_str(), sizeof(msg.sender) - 1);
    if (payload) strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    esp_now_send(mac, (uint8_t *)&msg, sizeof(msg));
}

// ─── Discovery ───────────────────────────────────────────────────────────────
void send_discovery() {
    Serial.println(F("[MASTER] Broadcasting #discovery..."));
    send_message(broadcast_mac, MSG_DISC, "");
}

void register_slave(const char *name, const uint8_t *mac) {
    int idx = find_slave_by_mac(mac);
    if (idx == -1) {
        if (slave_count >= MAX_SLAVES) {
            Serial.println(F("[MASTER] Limite de slaves atingido!"));
            return;
        }
        idx = slave_count++;
        char mac_str[18];
        mac_to_str(mac, mac_str, sizeof(mac_str));
        Serial.printf("[MASTER] Novo slave: %s  MAC: %s\n", name, mac_str);
    }
    strncpy(slaves[idx].name, name, sizeof(slaves[idx].name) - 1);
    memcpy(slaves[idx].mac, mac, 6);
    slaves[idx].online       = true;
    slaves[idx].last_seen_ms = millis();
    add_peer(mac);
}

// ─── Callbacks ESP-NOW ────────────────────────────────────────────────────────
void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(Message)) return;

    const Message *msg        = (const Message *)data;
    const uint8_t *sender_mac = info->src_addr;

    if (strcmp(msg->type, MSG_DISC_ACK) == 0) {
        register_slave(msg->sender, sender_mac);
        Serial.printf("[MASTER] Slave ONLINE: %s\n", msg->sender);

    } else if (strcmp(msg->type, MSG_PONG) == 0) {
        int idx = find_slave_by_mac(sender_mac);
        if (idx >= 0) {
            slaves[idx].last_seen_ms = millis();
            if (!slaves[idx].online) {
                slaves[idx].online = true;
                Serial.printf("[MASTER] Slave voltou ONLINE: %s\n", slaves[idx].name);
            }
        }

    } else if (strcmp(msg->type, MSG_DATA) == 0) {
        // Slave enviou dado/comando → payload no formato @nome#cmd
        Serial.printf("[SLAVE→MASTER] %s: %s\n", msg->sender, msg->payload);

    } else if (strcmp(msg->type, MSG_CMD) == 0) {
        // Raro: slave enviou CMD ao master
        Serial.printf("[RECV] De %s: %s\n", msg->sender, msg->payload);
    }
}

void on_data_sent(const uint8_t *mac, esp_now_send_status_t status) {
    // Descomente para depuração de envio:
    // Serial.printf("[MASTER] Send %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ─── Ping ────────────────────────────────────────────────────────────────────
void ping_slaves() {
    uint32_t now = millis();
    for (int i = 0; i < slave_count; i++) {
        SlaveInfo &s = slaves[i];
        if (!s.online) continue;

        if (now - s.last_seen_ms > OFFLINE_TIMEOUT_MS) {
            s.online = false;
            Serial.printf("[MASTER] Slave OFFLINE: %s\n", s.name);
            continue;
        }
        send_message(s.mac, MSG_PING, "#ping");
    }
}

// ─── Parser de Comandos Serial ───────────────────────────────────────────────
void handle_serial(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (!line.startsWith("@")) {
        Serial.println(F("[MASTER] Formato: @all#cmd  ou  @nome#cmd"));
        return;
    }

    int hash = line.indexOf('#');
    if (hash < 0) {
        Serial.println(F("[MASTER] Faltando '#' no comando"));
        return;
    }

    String target = line.substring(1, hash);  // all  ou  nome_slave
    String cmd    = line.substring(hash);     // #cmd (inclui o '#')

    if (target.equalsIgnoreCase("all")) {
        int sent = 0;
        for (int i = 0; i < slave_count; i++) {
            if (slaves[i].online) {
                send_message(slaves[i].mac, MSG_CMD, cmd.c_str());
                Serial.printf("[MASTER→%s] %s\n", slaves[i].name, cmd.c_str());
                sent++;
            }
        }
        if (sent == 0) Serial.println(F("[MASTER] Nenhum slave online"));

    } else {
        int idx = find_slave_by_name(target.c_str());
        if (idx < 0) {
            Serial.printf("[MASTER] Slave desconhecido: %s\n", target.c_str());
            return;
        }
        if (!slaves[idx].online) {
            Serial.printf("[MASTER] Slave offline: %s\n", target.c_str());
            return;
        }
        send_message(slaves[idx].mac, MSG_CMD, cmd.c_str());
        Serial.printf("[MASTER→%s] %s\n", target.c_str(), cmd.c_str());
    }
}

// ─── Lista de Slaves (comando 'list') ────────────────────────────────────────
void list_slaves() {
    Serial.printf("[MASTER] Slaves conhecidos: %d\n", slave_count);
    char mac_str[18];
    for (int i = 0; i < slave_count; i++) {
        mac_to_str(slaves[i].mac, mac_str, sizeof(mac_str));
        Serial.printf("  [%d] %s  MAC: %s  %s\n",
                      i, slaves[i].name, mac_str,
                      slaves[i].online ? "ONLINE" : "OFFLINE");
    }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    master_name = get_esp_name();
    Serial.printf("\n=== ESP-NOW MASTER ===\n");
    Serial.printf("Nome : %s\n", master_name.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("MAC  : %s\n", WiFi.macAddress().c_str());

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[MASTER] Falha ao iniciar ESP-NOW! Reiniciando..."));
        delay(1000);
        ESP.restart();
    }

    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    add_peer(broadcast_mac);

    send_discovery();
    last_discovery_ms = millis();
    last_ping_ms      = millis();

    Serial.println(F("Comandos: @all#cmd | @nome#cmd | list"));
    Serial.println(F("======================\n"));
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // Ping periódico
    if (now - last_ping_ms >= PING_INTERVAL_MS) {
        last_ping_ms = now;
        ping_slaves();
    }

    // Re-discovery se há slaves offline ou nenhum slave encontrado
    bool needs_discovery = (slave_count == 0);
    for (int i = 0; i < slave_count && !needs_discovery; i++) {
        if (!slaves[i].online) needs_discovery = true;
    }
    if (needs_discovery && (now - last_discovery_ms >= DISCOVERY_INTERVAL_MS)) {
        last_discovery_ms = now;
        send_discovery();
    }

    // Leitura serial
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.equalsIgnoreCase("list")) {
            list_slaves();
        } else {
            handle_serial(line);
        }
    }
}
