/**
 * ESP32 BLE ELM327 Emulator — Debugging & Interception Tool
 *
 * A BLE + Bluetooth Classic bridge that emulates an ELM327 OBD-II adapter.
 * Designed for:
 *   1. Intercepting OBD-II commands from apps (CarScanner, Torque, etc.)
 *   2. Bridging to ECU emulators (Ircama ELM327-emulator) via WiFi/TCP
 *   3. Reverse engineering ELM327 protocol communications
 *
 * Three simultaneous interfaces:
 *   - BLE 0xAE00  : Custom service for mechanical-assistant app
 *   - BLE 0xFFF0  : Standard OBD-II BLE UUIDs (CarScanner, Torque BLE)
 *   - BT Classic SPP : Legacy apps (Torque classic, older Android apps)
 *
 * All interfaces share the same pipeline:
 *   command → logger → AT dispatcher → TCP forward / local response
 *
 * Hardware: ESP32-WROOM-32 (only ESP32 variant with BT Classic + BLE)
 *
 * Copy config.h.example to config.h and fill in your WiFi/emulator settings.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "cmd_logger.h"
#include "at_handler.h"
#include "bt_classic.h"

// =====================================================================
// BLE Service UUIDs
// =====================================================================

// Custom service (mechanical-assistant app)
#define SERVICE_UUID_CUSTOM        "0000ae00-0000-1000-8000-00805f9b34fb"
#define WRITE_CHAR_UUID_CUSTOM     "0000ae01-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHAR_UUID_CUSTOM    "0000ae02-0000-1000-8000-00805f9b34fb"

// Standard OBD-II BLE UUIDs (compatible with CarScanner, Torque BLE, etc.)
#define SERVICE_UUID_STANDARD      "0000fff0-0000-1000-8000-00805f9b34fb"
#define WRITE_CHAR_UUID_STANDARD   "0000fff1-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHAR_UUID_STANDARD  "0000fff2-0000-1000-8000-00805f9b34fb"

// =====================================================================
// Constants
// =====================================================================

#define RESPONSE_BUFFER_SIZE 1024
#define SERIAL_CMD_BUF_SIZE  256
#define BLE_DEFAULT_MTU      20
#define BLE_ATT_OVERHEAD     3
#define HEAP_LOG_INTERVAL_MS 60000
#define WDT_TIMEOUT_S        30
#define TCP_MAX_RETRIES      3

// TCP connection state
enum TcpState {
  TCP_DISCONNECTED,
  TCP_CONNECTING,
  TCP_CONNECTED
};

// =====================================================================
// Global State
// =====================================================================

// BLE references
BLEServer *pServer = nullptr;
BLECharacteristic *pNotifyCharCustom   = nullptr;
BLECharacteristic *pNotifyCharStandard = nullptr;
bool deviceConnected = false;
bool needsAdvertisingRestart = false;
uint16_t negotiatedMtu = BLE_DEFAULT_MTU;

// TCP connection to Ircama emulator
WiFiClient elmClient;
TcpState tcpState = TCP_DISCONNECTED;

// Serial command buffer (for USB serial testing without BLE)
char serialCmdBuf[SERIAL_CMD_BUF_SIZE];
int serialCmdIdx = 0;

// Heap monitoring
unsigned long lastHeapLog = 0;

// Track which notify characteristic to respond on
BLECharacteristic *activeNotifyChar = nullptr;

// =====================================================================
// TCP Connection Management
// =====================================================================

/**
 * Attempt to connect to Ircama ELM327 emulator over TCP.
 * Implements exponential backoff: 500ms, 1000ms, 2000ms.
 * Returns true if connected.
 */
bool ensureELMConnection() {
  if (elmClient.connected()) {
    tcpState = TCP_CONNECTED;
    return true;
  }

  tcpState = TCP_CONNECTING;
  unsigned long delays[] = {500, 1000, 2000};

  for (int attempt = 0; attempt < TCP_MAX_RETRIES; attempt++) {
    Serial.printf("[TCP] Connecting to %s:%d (attempt %d/%d)...\n",
      ELM_HOST, ELM_PORT, attempt + 1, TCP_MAX_RETRIES);

    if (elmClient.connect(ELM_HOST, ELM_PORT)) {
      tcpState = TCP_CONNECTED;
      Serial.println("[TCP] Connected to ELM327 emulator!");
      return true;
    }

    if (attempt < TCP_MAX_RETRIES - 1) {
      delay(delays[attempt]);
    }
  }

  tcpState = TCP_DISCONNECTED;
  Serial.println("[TCP] All connection attempts failed.");
  return false;
}

/**
 * Read from the ELM emulator until '>' prompt or timeout.
 * Uses fixed char buffer instead of String to avoid heap fragmentation.
 */
int readELMResponse(char* buffer, size_t bufSize, unsigned long timeoutMs) {
  int idx = 0;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    while (elmClient.available() && idx < (int)(bufSize - 1)) {
      char c = elmClient.read();
      buffer[idx++] = c;

      if (c == '>') {
        buffer[idx] = '\0';
        return idx;
      }
    }

    // Check if TCP disconnected mid-read
    if (!elmClient.connected()) {
      tcpState = TCP_DISCONNECTED;
      Serial.println("[TCP] Connection lost during read");
      break;
    }

    delay(10);
  }

  // Timeout or disconnect — return what we have
  if (idx == 0) {
    strncpy(buffer, "NO DATA\r>", bufSize);
    idx = strlen(buffer);
  } else {
    buffer[idx] = '\0';
  }
  return idx;
}

/**
 * Remove echoed command from the response.
 * ELM emulator echoes "<cmd>\r" at the start of the response.
 */
void removeCommandEcho(char* response, const char* cmd) {
  // Build echo string: cmd + "\r"
  char echo[128];
  snprintf(echo, sizeof(echo), "%s\r", cmd);
  int echoLen = strlen(echo);

  char* found = strstr(response, echo);
  if (found) {
    // Shift the rest of the string over the echo
    memmove(found, found + echoLen, strlen(found + echoLen) + 1);
  }

  // Trim leading \r and \n
  while (response[0] == '\r' || response[0] == '\n') {
    memmove(response, response + 1, strlen(response));
  }
}

/**
 * Forward command to Ircama emulator and get response.
 */
int forwardToELM(const char* cmd, char* response, size_t respSize) {
  if (!ensureELMConnection()) {
    strncpy(response, "NO TCP\r>", respSize);
    return strlen(response);
  }

  // Append \r if not present
  char fullCmd[256];
  snprintf(fullCmd, sizeof(fullCmd), "%s", cmd);
  int len = strlen(fullCmd);
  if (len > 0 && fullCmd[len-1] != '\r' && fullCmd[len-1] != '\n') {
    fullCmd[len] = '\r';
    fullCmd[len+1] = '\0';
  }

  elmClient.print(fullCmd);

  unsigned long timeout = getCommandTimeout(cmd);
  int respLen = readELMResponse(response, respSize, timeout);

  removeCommandEcho(response, cmd);
  return strlen(response);
}

// =====================================================================
// BLE Response Sending
// =====================================================================

/**
 * Send response over BLE notifications, chunked by negotiated MTU.
 */
void sendBLEResponse(BLECharacteristic* pNotify, const char* response) {
  if (!pNotify || !deviceConnected) return;

  int maxChunk = negotiatedMtu - BLE_ATT_OVERHEAD;
  if (maxChunk < 20) maxChunk = 20;

  int len = strlen(response);
  int offset = 0;

  while (offset < len) {
    int chunkLen = min(maxChunk, len - offset);
    pNotify->setValue((uint8_t*)(response + offset), chunkLen);
    pNotify->notify();
    offset += chunkLen;
    delay(5);
  }
}

// =====================================================================
// Central Command Processing
// =====================================================================

/**
 * Process a command from any interface (BLE custom, BLE standard, BT SPP, Serial).
 * This is the unified pipeline that all interfaces share.
 */
void processCommand(const char* cmd, const char* source) {
  char response[RESPONSE_BUFFER_SIZE];
  bool isLocal = false;

  // Check for special commands first
  if (strcasecmp(cmd, "AT+LOG_DUMP") == 0) {
    String dump = cmdLogDump();
    if (strcmp(source, "BT_SPP") == 0) {
      btClassicSendResponse(dump.c_str());
    } else if (strcmp(source, "SERIAL") == 0) {
      Serial.print(dump);
    } else {
      sendBLEResponse(activeNotifyChar, dump.c_str());
    }
    cmdLogEntry(source, cmd, "[LOG_DUMP]", true);
    return;
  }

  if (strcasecmp(cmd, "AT+LOG_CLEAR") == 0) {
    cmdLogClear();
    snprintf(response, sizeof(response), "OK\r>");
    isLocal = true;
  } else {
    // AT command dispatcher
    bool tcpAvail = elmClient.connected();
    AtHandleResult result = handleATCommand(cmd, response, sizeof(response), tcpAvail);

    if (result == AT_HANDLED_LOCAL) {
      isLocal = true;
    } else {
      // Forward to Ircama
      forwardToELM(cmd, response, sizeof(response));
      isLocal = false;
    }
  }

  // Log the command/response pair
  cmdLogEntry(source, cmd, response, isLocal);

  // Send response to the originating interface
  if (strcmp(source, "BT_SPP") == 0) {
    btClassicSendResponse(response);
  } else if (strcmp(source, "SERIAL") == 0) {
    Serial.print(response);
    Serial.println();
  } else {
    // BLE response
    sendBLEResponse(activeNotifyChar, response);
  }
}

// =====================================================================
// BLE Callbacks
// =====================================================================

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) override {
    deviceConnected = true;
    Serial.println("[BLE] Central connected");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    negotiatedMtu = BLE_DEFAULT_MTU;
    Serial.println("[BLE] Central disconnected");
    needsAdvertisingRestart = true;
  }

  void onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) override {
    negotiatedMtu = param->mtu;
    Serial.printf("[BLE] MTU negotiated: %d (chunk size: %d)\n",
      negotiatedMtu, negotiatedMtu - BLE_ATT_OVERHEAD);
  }
};

// GAP event handler for BLE security
void ble_gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      Serial.println("[BLE] Passkey requested");
      esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, BLE_PASSKEY);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      Serial.printf("[BLE] Passkey for pairing: %06d\n", param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      Serial.printf("[BLE] Confirm passkey: %06d\n", param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      Serial.println("[BLE] Security requested");
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (param->ble_security.auth_cmpl.success) {
        Serial.println("[BLE] Authentication successful!");
      } else {
        Serial.printf("[BLE] Authentication failed. Reason: %d\n",
          param->ble_security.auth_cmpl.fail_reason);
      }
      break;
    default:
      break;
  }
}

// Write callback for custom service (0xAE00)
class WriteCallbackCustom : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      // Trim trailing CR/LF
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
      }
      activeNotifyChar = pNotifyCharCustom;
      processCommand(value.c_str(), "BLE/AE00");
    }
  }
};

// Write callback for standard service (0xFFF0)
class WriteCallbackStandard : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
      }
      activeNotifyChar = pNotifyCharStandard;
      processCommand(value.c_str(), "BLE/FFF0");
    }
  }
};

// =====================================================================
// BLE Security Configuration
// =====================================================================

void configureBLESecurity() {
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t key_size = 16;
  uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
}

// =====================================================================
// Wi-Fi Connection
// =====================================================================

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int maxRetries = 20;
  while (WiFi.status() != WL_CONNECTED && maxRetries > 0) {
    delay(1000);
    Serial.print(".");
    maxRetries--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed to connect! Running in standalone mode.");
  }
}

// =====================================================================
// Serial Command Input (for testing without BLE)
// =====================================================================

void pollSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialCmdIdx > 0) {
        serialCmdBuf[serialCmdIdx] = '\0';
        processCommand(serialCmdBuf, "SERIAL");
        serialCmdIdx = 0;
      }
    } else if (serialCmdIdx < SERIAL_CMD_BUF_SIZE - 1) {
      serialCmdBuf[serialCmdIdx++] = c;
    }
  }
}

// =====================================================================
// Setup
// =====================================================================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("============================================");
  Serial.println("  ESP32 BLE ELM327 Emulator v2.0");
  Serial.println("  Debugging & Interception Tool");
  Serial.println("============================================");

  // Enable watchdog timer (30s timeout)
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // 1. Connect to Wi-Fi
  connectWiFi();

  // 2. Init BLE (dual-mode: BLE + BT Classic share the Bluetooth controller)
  BLEDevice::init(BLE_DEVICE_NAME);

  // 3. Setup BLE Security
  BLEDevice::setCustomGapHandler(ble_gap_event_cb);
  configureBLESecurity();

  // 4. Create BLE server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // 5. Create custom GATT service (0xAE00) for mechanical-assistant
  BLEService *pServiceCustom = pServer->createService(SERVICE_UUID_CUSTOM);

  BLECharacteristic *pWriteCustom = pServiceCustom->createCharacteristic(
    WRITE_CHAR_UUID_CUSTOM,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pWriteCustom->setCallbacks(new WriteCallbackCustom());

  pNotifyCharCustom = pServiceCustom->createCharacteristic(
    NOTIFY_CHAR_UUID_CUSTOM,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pNotifyCharCustom->addDescriptor(new BLE2902());
  pServiceCustom->start();

  // 6. Create standard GATT service (0xFFF0) for CarScanner, Torque BLE, etc.
  BLEService *pServiceStandard = pServer->createService(SERVICE_UUID_STANDARD);

  BLECharacteristic *pWriteStandard = pServiceStandard->createCharacteristic(
    WRITE_CHAR_UUID_STANDARD,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pWriteStandard->setCallbacks(new WriteCallbackStandard());

  pNotifyCharStandard = pServiceStandard->createCharacteristic(
    NOTIFY_CHAR_UUID_STANDARD,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pNotifyCharStandard->addDescriptor(new BLE2902());
  pServiceStandard->start();

  // 7. Start BLE Advertising (both services)
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID_CUSTOM);
  pAdvertising->addServiceUUID(SERVICE_UUID_STANDARD);
  pAdvertising->setScanResponse(true);
  pServer->startAdvertising();

  // Preferred connection parameters for faster disconnect detection
  esp_ble_gap_set_prefer_conn_params(
    (esp_bd_addr_t){0, 0, 0, 0, 0, 0},
    0x10, 0x20, 0, 200  // min=20ms, max=40ms, latency=0, timeout=2s
  );

  // 8. Init Bluetooth Classic SPP
  btClassicInit(BT_CLASSIC_NAME);

  // Log startup info
  Serial.println("--------------------------------------------");
  Serial.printf("  BLE Device:    %s\n", BLE_DEVICE_NAME);
  Serial.printf("  BLE Custom:    0xAE00 (mechanical-assistant)\n");
  Serial.printf("  BLE Standard:  0xFFF0 (CarScanner/Torque)\n");
  Serial.printf("  BT Classic:    %s (SPP)\n", BT_CLASSIC_NAME);
  Serial.printf("  WiFi:          %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.printf("  ELM Emulator:  %s:%d\n", ELM_HOST, ELM_PORT);
  Serial.printf("  Free heap:     %d bytes\n", ESP.getFreeHeap());
  Serial.println("--------------------------------------------");
  Serial.println("Ready. Send commands via BLE, BT Classic, or Serial.");
  Serial.println();
}

// =====================================================================
// Main Loop
// =====================================================================

void loop() {
  // Feed the watchdog
  esp_task_wdt_reset();

  // Restart BLE advertising after disconnect (deferred from callback)
  if (needsAdvertisingRestart) {
    needsAdvertisingRestart = false;
    delay(100);
    pServer->startAdvertising();
    Serial.println("[BLE] Advertising restarted");
  }

  // Poll BT Classic for incoming commands
  btClassicPoll();

  // Poll USB Serial for commands (testing mode)
  pollSerialInput();

  // Periodic heap monitoring
  if (millis() - lastHeapLog > HEAP_LOG_INTERVAL_MS) {
    lastHeapLog = millis();
    Serial.printf("[SYS] Free heap: %d bytes | Min free: %d bytes\n",
      ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  delay(10);
}
