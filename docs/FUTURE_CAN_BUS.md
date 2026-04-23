# Future Upgrade: CAN Bus Physical Interface

## Overview

The ESP32-WROOM-32 has a built-in **TWAI controller** (Two-Wire Automotive Interface, compatible with ISO 11898-1 CAN). This enables direct communication with real vehicle CAN buses without an external CAN controller chip like MCP2515.

## Hardware Required

| Component | Purpose | Cost |
|-----------|---------|------|
| **CAN Transceiver** (TJA1050, TJA1051, or SN65HVD230) | Converts TWAI signals to CAN bus voltage levels | ~$1-2 USD |
| **OBD-II connector** (16-pin male) | Physical connection to vehicle diagnostic port | ~$3-5 USD |
| **Optional: MCP2515** (SPI) | Second CAN channel if needed | ~$3 USD |

The TJA1050/SN65HVD230 is preferred over MCP2515 because the ESP32's TWAI controller is native and more efficient (no SPI overhead).

## Wiring

```
ESP32           TJA1050         OBD-II Port
GPIO 21 (TX) -> TXD
GPIO 22 (RX) <- RXD
                CANH ---------> Pin 6 (CAN High)
                CANL ---------> Pin 14 (CAN Low)
3.3V ---------- VCC
GND ----------- GND ----------> Pin 4/5 (Chassis/Signal GND)
```

## Three Modes of Operation

### 1. CAN Gateway
BLE/BT app -> ESP32 -> CAN bus real

Replaces a commercial ELM327 adapter. The ESP32 translates AT commands into actual CAN frames and sends them on the bus.

### 2. CAN Sniffer
Passively reads CAN bus traffic and exposes it via BLE/BT/Serial for analysis.

### 3. CAN Injector (HIL Testing)
Injects CAN frames from Ircama emulator onto a physical bus for hardware-in-the-loop testing.

## Implementation Notes

### ESP32 TWAI API

```c
#include <driver/twai.h>

// Configure TWAI
twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_21, GPIO_NUM_22, TWAI_MODE_NORMAL);
twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();  // or 250KBITS
twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

twai_driver_install(&g_config, &t_config, &f_config);
twai_start();

// Send CAN frame
twai_message_t msg = {
    .identifier = 0x7DF,  // OBD-II broadcast
    .data_length_code = 8,
    .data = {0x02, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}  // RPM request
};
twai_transmit(&msg, pdMS_TO_TICKS(1000));

// Receive CAN frame
twai_message_t rx_msg;
twai_receive(&rx_msg, pdMS_TO_TICKS(1000));
```

### CAN Protocol Support

| Protocol | CAN Speed | CAN ID Type | Common In |
|----------|-----------|-------------|-----------|
| ISO 15765-4 | 500 kbps | 11-bit | Most modern vehicles |
| ISO 15765-4 | 250 kbps | 11-bit | Some vehicles |
| ISO 15765-4 | 500 kbps | 29-bit | Heavy trucks, some EU |
| ISO 15765-4 | 250 kbps | 29-bit | Heavy trucks |
| SAE J1939 | 250 kbps | 29-bit | Heavy trucks |

### AT Command Mapping for CAN Mode

When CAN hardware is detected, the AT dispatcher needs to translate:

| AT Command | CAN Action |
|-----------|------------|
| `ATSP6` | Set 500kbps 11-bit CAN |
| `ATSP7` | Set 250kbps 11-bit CAN |
| `ATSP8` | Set 500kbps 29-bit CAN |
| `ATSP9` | Set 250kbps 29-bit CAN |
| `ATSH 7E0` | Set CAN TX ID to 0x7E0 |
| `ATCRA 7E8` | Filter CAN RX for 0x7E8 |
| `01 0C` | Send CAN frame: ID=0x7DF, data=[02 01 0C 00 00 00 00 00] |

### Integration with routeCommand()

```
routeCommand(cmd):
  1. Check AT handler (local state commands)
  2. Check PID table (standalone mode)
  3. If CAN hardware detected → translate to CAN frames
  4. Else → forward to Ircama via TCP
```

## Estimated Implementation Effort

- **CAN driver + TWAI init**: M (Medium)
- **AT-to-CAN translation**: L (Large) — full ELM327 protocol is complex
- **CAN sniffer mode**: S (Small)
- **ISO-TP multi-frame over CAN**: L (Large)

## References

- [ESP32 TWAI Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html)
- [ELM327 Datasheet — Protocol Support](https://www.elmelectronics.com/wp-content/uploads/2016/07/ELM327DS.pdf)
- [CAN Bus Explained (CSS Electronics)](https://www.csselectronics.com/pages/can-bus-simple-intro-tutorial)
- [ISO 15765-2 ISO-TP Explained](https://www.embien.com/automotive-insights/demystifying-iso-15765-2-can-tp-docan-protocol)
