/**
 * bt_classic.h - Bluetooth Classic SPP (Serial Port Profile) interface
 *
 * Adds Bluetooth Classic support alongside BLE. Many real ELM327 adapters
 * use BT Classic SPP, so this enables compatibility with legacy apps like
 * Torque (classic mode) and older Android OBD-II apps.
 *
 * The ESP32-WROOM-32 is the only ESP32 variant that supports BT Classic.
 * ESP32-S3 and ESP32-C3 only support BLE.
 *
 * RAM impact: ~30KB extra for BT Classic stack. ESP32 has 520KB SRAM.
 */

#ifndef BT_CLASSIC_H
#define BT_CLASSIC_H

#include <Arduino.h>
#include <BluetoothSerial.h>

// Forward declaration - implemented in main sketch
extern void processCommand(const char* cmd, const char* source);

static BluetoothSerial btSerial;
static bool btClassicConnected = false;

// Buffer for accumulating BT Classic data
#define BT_CLASSIC_BUF_SIZE 256
static char btClassicBuf[BT_CLASSIC_BUF_SIZE];
static int btClassicBufIdx = 0;

/**
 * Callback for BT Classic connection events.
 */
static void btClassicCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  switch (event) {
    case ESP_SPP_SRV_OPEN_EVT:
      btClassicConnected = true;
      Serial.println("[BT_SPP] Client connected");
      break;
    case ESP_SPP_CLOSE_EVT:
      btClassicConnected = false;
      btClassicBufIdx = 0;
      Serial.println("[BT_SPP] Client disconnected");
      break;
    default:
      break;
  }
}

/**
 * Initialize Bluetooth Classic SPP.
 * Call after BLEDevice::init() — ESP32 supports dual-mode.
 */
static bool btClassicInit(const char* name) {
  btSerial.register_callback(btClassicCallback);
  if (!btSerial.begin(name)) {
    Serial.println("[BT_SPP] Failed to initialize Bluetooth Classic");
    return false;
  }
  Serial.printf("[BT_SPP] Bluetooth Classic SPP started as \"%s\"\n", name);
  return true;
}

/**
 * Send a response string over BT Classic SPP.
 * Unlike BLE, SPP has no chunking requirement — it's a serial stream.
 */
static void btClassicSendResponse(const char* response) {
  if (btClassicConnected && btSerial.hasClient()) {
    btSerial.print(response);
  }
}

/**
 * Poll for incoming BT Classic data.
 * Call from loop(). Accumulates characters until CR is received,
 * then processes the complete command.
 */
static void btClassicPoll() {
  while (btSerial.available()) {
    char c = btSerial.read();

    // Command terminator: CR or LF
    if (c == '\r' || c == '\n') {
      if (btClassicBufIdx > 0) {
        btClassicBuf[btClassicBufIdx] = '\0';
        processCommand(btClassicBuf, "BT_SPP");
        btClassicBufIdx = 0;
      }
      continue;
    }

    // Accumulate character
    if (btClassicBufIdx < BT_CLASSIC_BUF_SIZE - 1) {
      btClassicBuf[btClassicBufIdx++] = c;
    }
  }
}

#endif // BT_CLASSIC_H
