#!/usr/bin/env python3
"""
ESP32 ELM327 Emulator — Serial Test Harness

Tests the ESP32 firmware via USB serial, bypassing BLE entirely.
Validates AT command handling, TCP forwarding, and response formatting.

Usage:
  python3 esp32_serial_test.py                    # auto-detect port
  python3 esp32_serial_test.py /dev/ttyUSB0       # specific port
  python3 esp32_serial_test.py --standalone        # test without Ircama

Requirements:
  pip install pyserial
"""

import sys
import time
import serial
import serial.tools.list_ports


DEFAULT_BAUD = 115200
TIMEOUT = 5  # seconds per command


def find_esp32_port():
    """Auto-detect ESP32 serial port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        desc = (port.description or "").lower()
        if any(x in desc for x in ["cp210", "ch340", "ftdi", "usb", "uart"]):
            return port.device
    if ports:
        return ports[0].device
    return None


def send_command(ser, cmd):
    """Send a command and read the response until '>' prompt."""
    ser.write((cmd + "\r").encode("ascii"))
    ser.flush()

    response = b""
    start = time.time()
    while time.time() - start < TIMEOUT:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting)
            response += chunk
            if b">" in response:
                break
        time.sleep(0.05)

    return response.decode("ascii", errors="replace")


class TestResult:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []

    def check(self, name, condition, actual="", expected=""):
        if condition:
            self.passed += 1
            print(f"  PASS: {name}")
        else:
            self.failed += 1
            msg = f"  FAIL: {name}"
            if expected:
                msg += f" (expected: {expected!r}, got: {actual!r})"
            print(msg)
            self.errors.append(name)

    def summary(self):
        total = self.passed + self.failed
        print(f"\n{'='*50}")
        print(f"Results: {self.passed}/{total} passed, {self.failed} failed")
        if self.errors:
            print("Failed tests:")
            for e in self.errors:
                print(f"  - {e}")
        print(f"{'='*50}")
        return self.failed == 0


def test_at_commands(ser, results, standalone=False):
    """Test AT command handling."""
    print("\n--- AT Command Tests ---")

    # ATZ - Reset
    rsp = send_command(ser, "ATZ")
    results.check("ATZ responds",
                   "ELM327" in rsp,
                   rsp, "contains 'ELM327'")

    if standalone:
        results.check("ATZ shows [EMU] in standalone",
                       "[EMU]" in rsp,
                       rsp, "contains '[EMU]'")

    # ATI - Device ID
    rsp = send_command(ser, "ATI")
    results.check("ATI responds with ELM327",
                   "ELM327" in rsp,
                   rsp, "contains 'ELM327'")

    # ATE0 - Echo off
    rsp = send_command(ser, "ATE0")
    results.check("ATE0 responds OK",
                   "OK" in rsp,
                   rsp, "contains 'OK'")

    # ATE1 - Echo on
    rsp = send_command(ser, "ATE1")
    results.check("ATE1 responds OK",
                   "OK" in rsp,
                   rsp, "contains 'OK'")

    # ATH1 - Headers on
    rsp = send_command(ser, "ATH1")
    results.check("ATH1 responds OK",
                   "OK" in rsp,
                   rsp, "contains 'OK'")

    # ATRV - Read voltage
    rsp = send_command(ser, "ATRV")
    results.check("ATRV responds with voltage",
                   "V" in rsp,
                   rsp, "contains 'V'")

    # AT@1 - Device description
    rsp = send_command(ser, "AT@1")
    results.check("AT@1 responds",
                   len(rsp) > 3,
                   rsp, "non-empty response")


def test_special_commands(ser, results):
    """Test custom commands (LOG_DUMP, LOG_CLEAR)."""
    print("\n--- Special Command Tests ---")

    # Send a few commands first to populate the log
    send_command(ser, "ATI")
    send_command(ser, "ATE0")

    # AT+LOG_DUMP
    rsp = send_command(ser, "AT+LOG_DUMP")
    results.check("AT+LOG_DUMP returns log",
                   "CMD LOG" in rsp,
                   rsp, "contains 'CMD LOG'")
    results.check("AT+LOG_DUMP contains entries",
                   "ATI" in rsp or "ATE0" in rsp,
                   rsp, "contains previous commands")

    # AT+LOG_CLEAR
    rsp = send_command(ser, "AT+LOG_CLEAR")
    results.check("AT+LOG_CLEAR responds OK",
                   "OK" in rsp,
                   rsp, "contains 'OK'")

    # Verify log is empty after clear
    rsp = send_command(ser, "AT+LOG_DUMP")
    results.check("Log is empty after clear",
                   "0 entries" in rsp or "1 entries" in rsp,
                   rsp, "0 or 1 entries")


def test_obd_queries(ser, results, standalone=False):
    """Test OBD-II query forwarding (requires Ircama running)."""
    print("\n--- OBD Query Tests ---")

    if standalone:
        print("  (Skipping OBD queries in standalone mode)")
        return

    # 0100 - Supported PIDs
    rsp = send_command(ser, "0100")
    results.check("0100 responds (supported PIDs)",
                   "41 00" in rsp or "NO DATA" in rsp or "NO TCP" in rsp,
                   rsp, "valid OBD response or error")

    # 010C - Engine RPM
    rsp = send_command(ser, "010C")
    results.check("010C responds (RPM)",
                   "41 0C" in rsp or "NO DATA" in rsp or "NO TCP" in rsp,
                   rsp, "valid OBD response or error")

    # 010D - Vehicle speed
    rsp = send_command(ser, "010D")
    results.check("010D responds (speed)",
                   "41 0D" in rsp or "NO DATA" in rsp or "NO TCP" in rsp,
                   rsp, "valid OBD response or error")


def test_response_format(ser, results):
    """Test that responses end with '>' prompt."""
    print("\n--- Response Format Tests ---")

    rsp = send_command(ser, "ATI")
    results.check("Response ends with '>'",
                   rsp.rstrip().endswith(">"),
                   rsp, "ends with '>'")


def main():
    port = None
    standalone = "--standalone" in sys.argv

    # Parse arguments
    for arg in sys.argv[1:]:
        if not arg.startswith("--"):
            port = arg

    if not port:
        port = find_esp32_port()
        if not port:
            print("ERROR: No serial port found. Specify one: python3 esp32_serial_test.py /dev/ttyUSB0")
            sys.exit(1)

    print(f"ESP32 ELM327 Emulator — Serial Test Harness")
    print(f"Port: {port}")
    print(f"Baud: {DEFAULT_BAUD}")
    print(f"Mode: {'Standalone' if standalone else 'Bridge (Ircama required)'}")

    try:
        ser = serial.Serial(port, DEFAULT_BAUD, timeout=TIMEOUT)
        time.sleep(2)  # Wait for ESP32 to boot
        ser.reset_input_buffer()
    except serial.SerialException as e:
        print(f"ERROR: Could not open {port}: {e}")
        sys.exit(1)

    results = TestResult()

    test_at_commands(ser, results, standalone)
    test_special_commands(ser, results)
    test_obd_queries(ser, results, standalone)
    test_response_format(ser, results)

    ser.close()

    success = results.summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
