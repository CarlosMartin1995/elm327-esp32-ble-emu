/**
 * at_handler.h - AT Command Dispatcher (local handler + forward logic)
 *
 * Hybrid mode:
 *   - If TCP connected → forward AT commands to Ircama emulator
 *   - If TCP disconnected → respond locally with [EMU] suffix
 *   - Some commands (echo, headers, spaces) always update local state
 *
 * The ELM327 has ~60 AT commands. This handler categorizes them:
 *   LOCAL:       ATE, ATH, ATL, ATS, ATM, ATI, AT@1, ATRV, ATZ
 *   FORWARD:     ATSP, ATDP, ATSH, ATCRA, ATCAF, ATCFC, etc.
 *   PASS-THROUGH: OBD queries (01xx, 02xx, 09xx...)
 *   SPECIAL:     AT+LOG_DUMP, AT+LOG_CLEAR (custom commands)
 */

#ifndef AT_HANDLER_H
#define AT_HANDLER_H

#include <Arduino.h>

// ELM327 emulator identity
#define ELM_VERSION     "ELM327 v1.5"
#define ELM_EMU_VERSION "ELM327 v1.5 [EMU]"

// Local state flags (mirroring ELM327 behavior)
struct ElmState {
  bool echo;      // ATE0/E1
  bool headers;   // ATH0/H1
  bool linefeeds; // ATL0/L1
  bool spaces;    // ATS0/S1
  bool memory;    // ATM0/M1
};

static ElmState elmState = {
  .echo = true,
  .headers = false,
  .linefeeds = true,
  .spaces = true,
  .memory = false,
};

/**
 * Reset local ELM state to defaults (called on ATZ).
 */
static void elmStateReset() {
  elmState.echo = true;
  elmState.headers = false;
  elmState.linefeeds = true;
  elmState.spaces = true;
  elmState.memory = false;
}

/**
 * Result of AT command handling.
 */
enum AtHandleResult {
  AT_HANDLED_LOCAL,  // Response generated locally
  AT_FORWARD,        // Should be forwarded to Ircama
  AT_SPECIAL,        // Special command (LOG_DUMP, etc.)
};

/**
 * Try to handle an AT command locally.
 *
 * @param cmd     The command string (uppercase, trimmed)
 * @param response  Output buffer for local response
 * @param respSize  Size of output buffer
 * @param tcpAvail  Whether TCP connection to Ircama is available
 * @return AT_HANDLED_LOCAL if handled, AT_FORWARD if should be forwarded, AT_SPECIAL for custom commands
 */
static AtHandleResult handleATCommand(const char* cmd, char* response, size_t respSize, bool tcpAvail) {
  // Custom commands (always handled locally regardless of TCP)
  if (strcasecmp(cmd, "AT+LOG_DUMP") == 0) {
    return AT_SPECIAL; // Caller handles this
  }
  if (strcasecmp(cmd, "AT+LOG_CLEAR") == 0) {
    return AT_SPECIAL; // Caller handles this
  }

  // Commands that always update local state but may also forward
  // ATE0 / ATE1 - Echo control
  if (strcasecmp(cmd, "ATE0") == 0) {
    elmState.echo = false;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }
  if (strcasecmp(cmd, "ATE1") == 0) {
    elmState.echo = true;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATH0 / ATH1 - Headers
  if (strcasecmp(cmd, "ATH0") == 0) {
    elmState.headers = false;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }
  if (strcasecmp(cmd, "ATH1") == 0) {
    elmState.headers = true;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATL0 / ATL1 - Linefeeds
  if (strcasecmp(cmd, "ATL0") == 0) {
    elmState.linefeeds = false;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }
  if (strcasecmp(cmd, "ATL1") == 0) {
    elmState.linefeeds = true;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATS0 / ATS1 - Spaces
  if (strcasecmp(cmd, "ATS0") == 0) {
    elmState.spaces = false;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }
  if (strcasecmp(cmd, "ATS1") == 0) {
    elmState.spaces = true;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATM0 / ATM1 - Memory
  if (strcasecmp(cmd, "ATM0") == 0) {
    elmState.memory = false;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }
  if (strcasecmp(cmd, "ATM1") == 0) {
    elmState.memory = true;
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // Commands that respond differently based on TCP availability
  // ATZ - Reset
  if (strcasecmp(cmd, "ATZ") == 0) {
    elmStateReset();
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "\r\r%s\r\r>", ELM_EMU_VERSION);
    return AT_HANDLED_LOCAL;
  }

  // ATI - Device ID
  if (strcasecmp(cmd, "ATI") == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "%s\r>", ELM_EMU_VERSION);
    return AT_HANDLED_LOCAL;
  }

  // AT@1 - Device description
  if (strcasecmp(cmd, "AT@1") == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "ESP32 BLE ELM327 Bridge [EMU]\r>");
    return AT_HANDLED_LOCAL;
  }

  // AT@2 - Device ID
  if (strcasecmp(cmd, "AT@2") == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "ESP32-EMU\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATRV - Read voltage
  if (strcasecmp(cmd, "ATRV") == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "12.6V\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATSP - Set Protocol (always forward if TCP available)
  if (strncasecmp(cmd, "ATSP", 4) == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // ATDP / ATDPN - Describe Protocol
  if (strcasecmp(cmd, "ATDP") == 0 || strcasecmp(cmd, "ATDPN") == 0) {
    if (tcpAvail) return AT_FORWARD;
    snprintf(response, respSize, "AUTO\r>");
    return AT_HANDLED_LOCAL;
  }

  // Any other AT command
  if (strncasecmp(cmd, "AT", 2) == 0) {
    if (tcpAvail) return AT_FORWARD;
    // Unknown AT command in standalone mode
    snprintf(response, respSize, "OK\r>");
    return AT_HANDLED_LOCAL;
  }

  // Not an AT command — it's an OBD query, always forward
  return AT_FORWARD;
}

/**
 * Determine the appropriate timeout for a command.
 * AT commands: 2s, PID queries: 5s, multi-frame (VIN/DTC): 10s
 */
static unsigned long getCommandTimeout(const char* cmd) {
  // AT commands are fast
  if (strncasecmp(cmd, "AT", 2) == 0) {
    return 2000;
  }
  // Mode 09 (vehicle info, VIN) and Mode 03/07 (DTCs) can be multi-frame
  if (cmd[0] == '0' && (cmd[1] == '9' || cmd[1] == '3' || cmd[1] == '7')) {
    return 10000;
  }
  // Standard PID queries
  return 5000;
}

#endif // AT_HANDLER_H
