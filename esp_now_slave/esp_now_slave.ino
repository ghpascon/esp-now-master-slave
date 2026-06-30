/*
 * ESP-NOW Slave
 * ─────────────
 * Responsabilidades:
 *  - Aguarda o master enviar #discovery (MSG_DISC)
 *  - Registra o master como peer e responde com DISC_ACK
 *  - Responde a #ping com #pong
 *  - Executa comandos recebidos do master (MSG_CMD)
 *  - Marca master como offline após OFFLINE_TIMEOUT_MS sem receber nada
 *  - Ao redetectar discovery do master, restabelece conexão automaticamente
 *  - Aceita comandos via Serial para enviar ao master: #cmd
 *    (enviado como payload @slave_name#cmd no MSG_DATA)
 *
 * Protocolo (struct Message):
 *   type    – tipo da mensagem (DISC, DISC_ACK, PING, PONG, CMD, DATA)
 *   sender  – nome ESP do remetente
 *   payload – conteúdo (comando, resposta, etc.)
 */

#include <esp_now.h>
#include <WiFi.h>

// ─── Configurações ───────────────────────────────────────────────────────────
#define OFFLINE_TIMEOUT_MS   15000  // tempo sem ouvir o master → offline

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

// ─── Globais ─────────────────────────────────────────────────────────────────
String   slave_name;
uint8_t  master_mac[6]    = {0};
bool     master_known     = false;
uint32_t last_master_seen = 0;

// ─── Utilitários ─────────────────────────────────────────────────────────────
String get_esp_name() {
    uint64_t chipid = ESP.getEfuseMac();
    char id_str[13];
    sprintf(id_str, "%012llX", chipid);
    return "SMTX-" + String(id_str);
}

void add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void remove_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
}

// ─── Envio de Mensagem ───────────────────────────────────────────────────────
void send_message(const uint8_t *mac, const char *type, const char *payload) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.type,   type,             sizeof(msg.type)    - 1);
    strncpy(msg.sender, slave_name.c_str(), sizeof(msg.sender) - 1);
    if (payload) strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    esp_now_send(mac, (uint8_t *)&msg, sizeof(msg));
}

// ─── Envio para o Master ─────────────────────────────────────────────────────
void send_to_master(const char *cmd) {
    // cmd deve começar com '#', ex: "#hello"
    if (!master_known) {
        Serial.println(F("[SLAVE] Master não encontrado ainda."));
        return;
    }
    // Monta payload no formato @slave_name#cmd
    char payload[220];
    snprintf(payload, sizeof(payload), "@%s%s", slave_name.c_str(), cmd);
    send_message(master_mac, MSG_DATA, payload);
    Serial.printf("[SLAVE→MASTER] %s\n", payload);
}

// ─── Handlers de Comando ─────────────────────────────────────────────────────
void handle_command(const char *cmd) {
    Serial.printf("[SLAVE] Comando recebido: %s\n", cmd);

    if (strcmp(cmd, "#ping") == 0) {
        // ping via CMD (não deve ocorrer normalmente, mas tratamos)
        send_to_master("#pong");

    } else if (strcmp(cmd, "#get_info") == 0) {
        char info[180];
        snprintf(info, sizeof(info), "#info:heap=%u,name=%s",
                 ESP.getFreeHeap(), slave_name.c_str());
        send_to_master(info);

    } else if (strcmp(cmd, "#restart") == 0) {
        Serial.println(F("[SLAVE] Reiniciando por comando do master..."));
        delay(200);
        ESP.restart();

    } else {
        // Comando desconhecido: ecoa de volta ao master como resposta
        char resp[220];
        snprintf(resp, sizeof(resp), "#ack:%s", cmd);
        send_to_master(resp);
    }
}

// ─── Callbacks ESP-NOW ────────────────────────────────────────────────────────
void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(Message)) return;

    const Message *msg        = (const Message *)data;
    const uint8_t *sender_mac = info->src_addr;

    if (strcmp(msg->type, MSG_DISC) == 0) {
        // Master encontrado (ou re-encontrado)
        bool is_new = !master_known || memcmp(master_mac, sender_mac, 6) != 0;

        if (master_known) remove_peer(master_mac); // limpa peer anterior se diferente
        add_peer(sender_mac);
        memcpy(master_mac, sender_mac, 6);
        master_known     = true;
        last_master_seen = millis();

        if (is_new) {
            Serial.printf("[SLAVE] Master detectado: %s\n", msg->sender);
        }
        send_message(sender_mac, MSG_DISC_ACK, "");

    } else if (strcmp(msg->type, MSG_PING) == 0) {
        last_master_seen = millis();
        send_message(sender_mac, MSG_PONG, "#pong");

    } else if (strcmp(msg->type, MSG_CMD) == 0) {
        last_master_seen = millis();
        handle_command(msg->payload);

    } else if (strcmp(msg->type, MSG_DATA) == 0) {
        // Master enviou dado (incomum nessa direção)
        Serial.printf("[MASTER→SLAVE] %s\n", msg->payload);
    }
}

void on_data_sent(const uint8_t *mac, esp_now_send_status_t status) {
    // Descomente para depuração de envio:
    // Serial.printf("[SLAVE] Send %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    slave_name = get_esp_name();
    Serial.printf("\n=== ESP-NOW SLAVE ===\n");
    Serial.printf("Nome: %s\n", slave_name.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("MAC : %s\n", WiFi.macAddress().c_str());

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[SLAVE] Falha ao iniciar ESP-NOW! Reiniciando..."));
        delay(1000);
        ESP.restart();
    }

    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    Serial.println(F("Aguardando discovery do master..."));
    Serial.println(F("Serial: #cmd  (ex: #hello)"));
    Serial.println(F("====================\n"));
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // Verifica timeout do master
    if (master_known && (now - last_master_seen > OFFLINE_TIMEOUT_MS)) {
        Serial.println(F("[SLAVE] Master OFFLINE. Aguardando re-discovery..."));
        remove_peer(master_mac);
        master_known = false;
        memset(master_mac, 0, sizeof(master_mac));
    }

    // Leitura serial → enviar comando ao master
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return;

        if (line.startsWith("#")) {
            send_to_master(line.c_str());
        } else {
            Serial.println(F("[SLAVE] Use '#cmd' para enviar ao master. Ex: #hello"));
        }
    }
}
