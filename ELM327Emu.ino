/**
 * ESP32 BLE "ELM327" Emulator with Wi-Fi bridge to local ELM327 emulator
 *
 * The ESP32 advertises a BLE service (0xAE00) with:
 *    - 0xAE01 (Write/WriteNoResponse): phone writes ASCII ELM commands
 *    - 0xAE02 (Notify): phone subscribes to get ASCII ELM responses
 *
 * Adjust SSID, PASSWORD, ELM_HOST, ELM_PORT as needed.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLE2902.h>

// ---------- Wi-Fi Credentials ------------
const char* WIFI_SSID     = "YOUR_SSID"; // Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "YOUR_PASSWORD"; // Replace with your Wi-Fi password

// ---------- ELM Emulator TCP -------------
const char* ELM_HOST = "YOUR_ELM_HOST"; // Replace with your ELM emulator IP (your local machine)
const uint16_t ELM_PORT = 35000;

// ---------- BLE ELM327 Service/Chars -----
#define SERVICE_UUID               "0000ae00-0000-1000-8000-00805f9b34fb"
#define WRITE_CHARACTERISTIC_UUID  "0000ae01-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHARACTERISTIC_UUID "0000ae02-0000-1000-8000-00805f9b34fb"
#define PASSKEY 123456  // 6-digit passkey for BLE

// BLE references
BLECharacteristic *pWriteChar  = nullptr;
BLECharacteristic *pNotifyChar = nullptr;

// Track connection and notification status
bool deviceConnected = false;

// We'll use a global WiFiClient for the ELM emulator
WiFiClient elmClient;

/**
 * Attempt to connect to local ELM327 emulator over TCP if not connected
 * Return true if connected, false otherwise
 */
bool ensureELMConnection() {
  if (elmClient.connected()) {
    return true; // Already connected
  }
  Serial.print("[TCP] Connecting to ELM327 emulator at ");
  Serial.print(ELM_HOST);
  Serial.print(":");
  Serial.println(ELM_PORT);

  if (!elmClient.connect(ELM_HOST, ELM_PORT)) {
    Serial.println("[TCP] Connection failed.");
    return false;
  }
  Serial.println("[TCP] Connected to ELM327 emulator!");
  return true;
}

/**
 * Read from the ELM emulator until we see the '>' prompt or time out.
 * Return the complete response string.
 */
String readELMResponse() {
  String response;
  unsigned long start = millis();

  while (millis() - start < 10000) { // up to 10 seconds
    while (elmClient.available()) {
      char c = elmClient.read();
      response += c;

      // ELM typically ends a response with '>'
      if (c == '>') {
        return response;
      }
    }
    // small delay to let more data arrive
    delay(10);
  }

  // If we exit the loop, we never saw '>'
  // Return whatever we have, or "NO DATA" if empty
  if (response.length() == 0) {
    response = "NO DATA\r>";
  }
  return response;
}

/**
 * Sends the full response over BLE by splitting it into chunks.
 */
void sendBLEResponse(const String &response) {
  const int maxChunkSize = 20; // Adjust if MTU negotiation allows for larger packets
  int len = response.length();
  int offset = 0;
  
  Serial.print("[BLE] Sending response in chunks, total length: ");
  Serial.println(len);
  
  while (offset < len) {
    // Get a substring of up to maxChunkSize characters.
    String chunk = response.substring(offset, min(offset + maxChunkSize, len));
    pNotifyChar->setValue(chunk);
    pNotifyChar->notify();
    
    Serial.print("[BLE] Sent chunk: ");
    Serial.println(chunk);
    
    offset += maxChunkSize;
    // Small delay to allow the central to process notifications
    delay(5);
  }
}

/**
 * Forward the ASCII command to ELM emulator over TCP, read response
 * Return response as String
 */
String forwardToELM(const String &cmd) {
  if (!ensureELMConnection()) {
    return "NO TCP\r>"; 
  }

  // The ELM emulator expects commands to end with \r or \n
  // If not present, append \r
  // This ensures each command is recognized properly
  String fullCmd = cmd;
  if (!fullCmd.endsWith("\r") && !fullCmd.endsWith("\n")) {
    fullCmd += "\r";
  }

  // Send command
  elmClient.print(fullCmd);

  // Now read until we get the '>' or time out
  String response = readELMResponse();

  return response;
}

// Callbacks for the BLE Server (connect/disconnect events)
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] Central connected");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("[BLE] Central disconnected");
    // Restart advertising so another device can connect
    pServer->startAdvertising();
  }
};

// Handle BLE Security (passkey, bonding)
class MySecurityCallbacks : public BLESecurityCallbacks {
public:
  uint32_t onPassKeyRequest() {
    Serial.println("[BLE] Passkey requested");
    return PASSKEY;
  }

  void onPassKeyNotify(uint32_t pass_key) {
    Serial.print("[BLE] Passkey for pairing: ");
    Serial.println(pass_key);
  }

  bool onConfirmPIN(uint32_t pass_key) {
    Serial.print("[BLE] Confirm passkey: ");
    Serial.println(pass_key);
    // Return true to accept. 
    return true;
  }

  bool onSecurityRequest() {
    Serial.println("[BLE] Security requested");
    return true; // Accept connection attempts that require security
  }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) {
    if (auth_cmpl.success) {
      Serial.println("[BLE] Authentication successful! Connection is now secured.");
    } else {
      Serial.print("[BLE] Authentication failed. Reason: ");
      Serial.println(auth_cmpl.fail_reason);
    }
  }
};

String removeCommandEcho(const String& response, const String& cmd) {
  // Copy for editing
  String clean = response;

  // The ELM emulator typically echoes "<cmd>\r" at the start.
  // So let's build that substring:
  String echo = cmd + "\r";

  // 1. Remove the command + "\r" if found.
  int idx = clean.indexOf(echo);
  if (idx >= 0) {
    clean.remove(idx, echo.length());
  }

  // 2. Trim any leading \r or \n left behind after removing
  while (clean.startsWith("\r") || clean.startsWith("\n")) {
    clean.remove(0, 1);
  }

  return clean;
}

class WriteCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    // ASCII command from the phone
    String value = pCharacteristic->getValue();
    if (!value.isEmpty()) {
      String cmd = String(value.c_str());
      Serial.print("[BLE] Write received: ");
      Serial.println(cmd);

      // Forward the ASCII command to local ELM emulator
      String response = forwardToELM(cmd);

      // Remove the echoed command from the response ---
      String cleanedResponse = removeCommandEcho(response, cmd);

      sendBLEResponse(cleanedResponse);

      Serial.print("[BLE] Notified response: ");
      Serial.println(cleanedResponse);
    }
  }
};

void configureBLESecurity(){
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;          
  uint8_t key_size = 16;     
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint32_t passkey = PASSKEY;
  uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_DISABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
}

// Connect to Wi-Fi
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int maxRetries = 20;
  while (WiFi.status() != WL_CONNECTED && maxRetries > 0) {
    delay(1000);
    Serial.print(".");
    maxRetries--;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Failed to connect!");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 BLE ELM327 Emulator (mock)...");

  // 1. Connect to Wi-Fi
  connectWiFi();
  
  // 2. Init BLE
  BLEDevice::init("ESP32-ELM327-Emu");
  BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);

  // 3. Setup BLE Security
  configureBLESecurity();

  // 4. Create BLE server
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // 5. Create GATT service and characteristics
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Write Characteristic (0xAE01) => phone writes commands
  pWriteChar = pService->createCharacteristic(
    WRITE_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pWriteChar->setCallbacks(new WriteCharacteristicCallbacks());

  // Notify Characteristic (0xAE02) => phone subscribes to get data
  pNotifyChar = pService->createCharacteristic(
    NOTIFY_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  // Add CCC descriptor so phone can enable notifications
  pNotifyChar->addDescriptor(new BLE2902());

  pService->start();

  // 6. Start Advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pServer->startAdvertising();

  Serial.println("BLE ELM327 Emulator is now advertising (passkey = 123456)...");
}

void loop() {
  // For a real “car simulator,” you could periodically push new data,
  // or handle advanced commands. For now, we only respond to write requests.
  delay(1000);
}
