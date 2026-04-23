/**
 * cmd_logger.h - Command Logger / Sniffer for OBD-II reverse engineering
 *
 * Circular buffer that records the last CMD_LOG_MAX_ENTRIES command/response
 * pairs with timestamps and source interface. Essential for intercepting and
 * reverse-engineering OBD-II apps like CarScanner, Torque, etc.
 *
 * Usage:
 *   cmdLogEntry("BLE/FFF0", "010C\r", "41 0C 1A F8\r>", false);
 *   cmdLogEntry("BT_SPP",  "ATZ\r",  "ELM327 v1.5 [EMU]\r\r>", true);
 *   cmdLogDump(Serial);
 *   cmdLogClear();
 */

#ifndef CMD_LOGGER_H
#define CMD_LOGGER_H

#include <Arduino.h>

#define CMD_LOG_MAX_ENTRIES 200
#define CMD_LOG_MAX_CMD_LEN 64
#define CMD_LOG_MAX_RSP_LEN 128

struct CmdLogEntry {
  unsigned long timestamp;
  char source[12];    // "BLE/AE00", "BLE/FFF0", "BT_SPP", "SERIAL"
  char command[CMD_LOG_MAX_CMD_LEN];
  char response[CMD_LOG_MAX_RSP_LEN];
  bool localResponse; // true = handled locally, false = forwarded to emulator
};

static CmdLogEntry cmdLog[CMD_LOG_MAX_ENTRIES];
static int cmdLogHead = 0;
static int cmdLogCount = 0;

/**
 * Add a command/response pair to the log.
 * Prints to Serial in real-time for monitoring.
 */
static void cmdLogEntry(const char* source, const char* cmd, const char* rsp, bool local) {
  CmdLogEntry& entry = cmdLog[cmdLogHead];
  entry.timestamp = millis();
  strncpy(entry.source, source, sizeof(entry.source) - 1);
  entry.source[sizeof(entry.source) - 1] = '\0';
  strncpy(entry.command, cmd, sizeof(entry.command) - 1);
  entry.command[sizeof(entry.command) - 1] = '\0';
  strncpy(entry.response, rsp, sizeof(entry.response) - 1);
  entry.response[sizeof(entry.response) - 1] = '\0';
  entry.localResponse = local;

  cmdLogHead = (cmdLogHead + 1) % CMD_LOG_MAX_ENTRIES;
  if (cmdLogCount < CMD_LOG_MAX_ENTRIES) cmdLogCount++;

  // Real-time serial output
  Serial.printf("[%lums] %-10s APP> \"%s\" | %s< \"%s\"\n",
    entry.timestamp,
    source,
    cmd,
    local ? "LOCAL" : "EMU",
    rsp);
}

/**
 * Dump the entire log to a Print stream (Serial, BLE, etc.)
 * Returns the dump as a String for sending over BLE.
 */
static String cmdLogDump() {
  String dump = "=== CMD LOG (" + String(cmdLogCount) + " entries) ===\r\n";

  int start;
  if (cmdLogCount < CMD_LOG_MAX_ENTRIES) {
    start = 0;
  } else {
    start = cmdLogHead; // oldest entry
  }

  for (int i = 0; i < cmdLogCount; i++) {
    int idx = (start + i) % CMD_LOG_MAX_ENTRIES;
    CmdLogEntry& e = cmdLog[idx];
    dump += "[" + String(e.timestamp) + "ms] ";
    dump += String(e.source) + " ";
    dump += "APP>\"" + String(e.command) + "\" | ";
    dump += String(e.localResponse ? "LOCAL" : "EMU");
    dump += "<\"" + String(e.response) + "\"\r\n";
  }

  dump += "=== END LOG ===\r\n>";
  return dump;
}

/**
 * Clear the log buffer.
 */
static void cmdLogClear() {
  cmdLogHead = 0;
  cmdLogCount = 0;
  Serial.println("[LOG] Command log cleared");
}

#endif // CMD_LOGGER_H
