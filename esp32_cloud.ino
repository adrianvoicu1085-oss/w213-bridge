/*
 * ═══════════════════════════════════════════════════════════════
 *  Mercedes W213 – ESP32 Firmware cu Cloud WebSocket
 *  Arduino Nano ESP32 + CAN SPI Click 3.3V
 *
 *  Pini:
 *    CS#   → D10 (GPIO10)
 *    SCK   → D13 (GPIO13)
 *    MISO  → D12 (GPIO12)
 *    MOSI  → D11 (GPIO11)
 *    INT#  → D8  (GPIO8)
 *    RST#  → D9  (GPIO9)
 *    VCC   → 3.3V
 *    GND   → GND
 *
 *  Librarii:
 *    - mcp_can       by coryjfowler
 *    - ArduinoJson   by Benoit Blanchon
 *    - WebSockets    by Markus Sattler  ← NOU
 * ═══════════════════════════════════════════════════════════════
 */

#include <SPI.h>
#include <mcp_can.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

// ─── WiFi – reteaua din masina / hotspot ──────────────────────
const char* WIFI_SSID     = "RETEAUA_DIN_MASINA";   // ← schimba
const char* WIFI_PASSWORD = "PAROLA_WIFI";            // ← schimba

// ─── Cloud server ─────────────────────────────────────────────
// Dupa deploy pe Railway/Render, pune URL-ul aici
// ex: "w213-bridge.railway.app"
const char* CLOUD_HOST = "w213-bridge.railway.app";  // ← schimba
const int   CLOUD_PORT = 443;                         // 443 pentru WSS
const char* CLOUD_PATH = "/";

// ─── Pini MCP2515 ─────────────────────────────────────────────
#define CAN_CS_PIN  10
#define CAN_INT_PIN  8
#define CAN_RST_PIN  9

// ─── ID-uri CAN W213 ──────────────────────────────────────────
// Inlocuieste cu ID-urile gasite prin sniffing!
#define CAN_ENGINE_START_ID  0x01D0
#define CAN_ENGINE_STOP_ID   0x01D0
#define CAN_LOCK_ID          0x02A0
#define CAN_UNLOCK_ID        0x02A0
#define CAN_CLIMATE_ID       0x03E0

MCP_CAN          CAN(CAN_CS_PIN);
WebSocketsClient ws;

bool canReady  = false;
bool wsReady   = false;
bool engineOn  = false;
bool locked    = true;
bool acOn      = false;
float acTemp   = 22.0;
int  acFan     = 3;

unsigned long lastStateReport = 0;

// ─── Trimite frame CAN ────────────────────────────────────────
bool sendFrame(uint32_t id, uint8_t* data, uint8_t len) {
  if (!canReady) { Serial.println("[CAN] Bus indisponibil"); return false; }
  byte err = CAN.sendMsgBuf(id, 0, len, data);
  if (err == CAN_OK) {
    Serial.printf("[CAN TX] ID=0x%03X\n", id);
    return true;
  }
  Serial.printf("[CAN ERR] %d\n", err);
  return false;
}

void sendWakeup() {
  uint8_t wake[4] = {0x00, 0x00, 0x00, 0x00};
  sendFrame(0x000, wake, 4);
  delay(100);
}

// ─── Trimite starea masinii la server ─────────────────────────
void reportState() {
  StaticJsonDocument<256> doc;
  doc["type"]          = "state";
  JsonObject data      = doc.createNestedObject("data");
  data["engine"]       = engineOn;
  data["locked"]       = locked;
  data["ac_on"]        = acOn;
  data["ac_temp"]      = acTemp;
  data["ac_fan"]       = acFan;
  data["can_ready"]    = canReady;
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);
}

// ─── Executa comanda primita de la server ─────────────────────
void executeCommand(const String& payload) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;

  const char* action = doc["action"];
  Serial.printf("[CMD] %s\n", action);

  if (strcmp(action, "engine_start") == 0) {
    sendWakeup();
    uint8_t ezs[8] = {0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t ecm[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(CAN_ENGINE_START_ID, ezs, 8); delay(50);
    sendFrame(0x0C0, ecm, 8);
    engineOn = true;

  } else if (strcmp(action, "engine_stop") == 0) {
    uint8_t ezs[8] = {0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t ecm[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(CAN_ENGINE_STOP_ID, ezs, 8); delay(50);
    sendFrame(0x0C0, ecm, 8);
    engineOn = false;

  } else if (strcmp(action, "lock") == 0) {
    sendWakeup();
    uint8_t data[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(CAN_LOCK_ID, data, 8);
    locked = true;

  } else if (strcmp(action, "unlock") == 0) {
    sendWakeup();
    uint8_t data[8] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(CAN_UNLOCK_ID, data, 8);
    locked = false;

  } else if (strcmp(action, "climate") == 0) {
    acOn   = doc["on"]   | acOn;
    acTemp = doc["temp"] | acTemp;
    acFan  = doc["fan"]  | acFan;
    acFan  = constrain(acFan, 1, 8);
    uint8_t data[8] = {
      (uint8_t)(acOn ? 0x01 : 0x00),
      (uint8_t)(acTemp * 2),
      (uint8_t)acFan,
      0x00, 0x00, 0x00, 0x00, 0x00
    };
    sendFrame(CAN_CLIMATE_ID, data, 8);

  } else if (strcmp(action, "sniff") == 0) {
    // Trimite 20 mesaje CAN capturate
    StaticJsonDocument<1024> snap;
    snap["type"] = "sniff";
    JsonArray frames = snap.createNestedArray("frames");
    unsigned long t = millis();
    while (millis() - t < 1000 && frames.size() < 20) {
      if (CAN_MSGAVAIL == CAN.checkReceive()) {
        uint32_t id; uint8_t len; uint8_t buf[8];
        CAN.readMsgBuf(&id, &len, buf);
        JsonObject f = frames.createNestedObject();
        f["id"] = id;
        String hex = "";
        for (int i = 0; i < len; i++) { if (buf[i] < 16) hex += "0"; hex += String(buf[i], HEX); if (i < len-1) hex += " "; }
        f["data"] = hex;
      }
    }
    String out; serializeJson(snap, out);
    ws.sendTXT(out);
    return; // nu trimite state dupa sniff
  }

  reportState();
}

// ─── WebSocket events ─────────────────────────────────────────
void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Conectat la server cloud!");
      wsReady = true;
      reportState(); // trimite starea initiala
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Deconectat de la server cloud");
      wsReady = false;
      break;

    case WStype_TEXT:
      executeCommand(String((char*)payload));
      break;

    case WStype_PING:
      Serial.println("[WS] Ping primit");
      break;

    default: break;
  }
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Mercedes W213 ESP32 Cloud Client ===");

  // WiFi
  Serial.printf("Conectare la: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] WiFi conectat! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[ERR] WiFi esuat! Verifica credentialele.");
  }

  // MCP2515
  Serial.println("Initializare MCP2515...");
  if (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_10MHZ) == CAN_OK) {
    CAN.setMode(MCP_NORMAL);
    pinMode(CAN_INT_PIN, INPUT);
    canReady = true;
    Serial.println("[OK] MCP2515 @ 500kbps / 10MHz");
  } else {
    Serial.println("[ERR] MCP2515 esuat!");
  }

  // WebSocket catre server cloud
  Serial.printf("Conectare la cloud: %s\n", CLOUD_HOST);
  ws.beginSSL(CLOUD_HOST, CLOUD_PORT, CLOUD_PATH);
  ws.onEvent(onWebSocketEvent);
  ws.setReconnectInterval(5000); // reconectare la 5s daca cade
  ws.enableHeartbeat(25000, 3000, 2);

  Serial.println("========================================\n");
}

// ─── Loop ─────────────────────────────────────────────────────
void loop() {
  ws.loop();

  // Citire mesaje CAN incoming
  if (canReady && CAN_MSGAVAIL == CAN.checkReceive()) {
    uint32_t id; uint8_t len; uint8_t buf[8];
    CAN.readMsgBuf(&id, &len, buf);
    // Proceseaza mesaje incoming daca e nevoie
  }

  // Raporteaza starea la fiecare 30s
  if (wsReady && millis() - lastStateReport > 30000) {
    reportState();
    lastStateReport = millis();
  }

  delay(1);
}
