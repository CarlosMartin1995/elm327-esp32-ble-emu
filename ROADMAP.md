# ESP32 BLE ELM327 — Roadmap

## Status: Fase 1+2 Implementadas

### Implementado (v2.0)

- [x] **1.1** Gestión de configuración (`config.h` gitignored)
- [x] **1.2** Negociación MTU dinámica
- [x] **1.3** Reconexión TCP con backoff exponencial (3 reintentos)
- [x] **1.4** Timeout configurable por tipo de comando (AT: 2s, PID: 5s, multi-frame: 10s)
- [x] **1.5** Watchdog timer (30s) + monitoreo de heap (cada 60s)
- [x] **2.1** Buffers fijos (char[]) en vez de String (evita heap fragmentation)
- [x] **2.2** UUIDs BLE estándar 0xFFF0 (CarScanner, Torque) + custom 0xAE00
- [x] **2.3** Write-With-Response support para apps de terceros
- [x] **2.4** Command logger/sniffer (buffer circular 200 entries, AT+LOG_DUMP/CLEAR)
- [x] **2.5** AT Command Dispatcher híbrido (local con [EMU] / forward a Ircama)
- [x] **2.6** Bluetooth Classic SPP (compatible con apps legacy)
- [x] **2.7** Test harness serial (Python via USB, testing sin BLE)

### Pendiente: Fase 3 — Modo Standalone

- [ ] **3.1** Tabla de PIDs embebida con escenarios (healthy_car, check_engine, overheating)
- [ ] **3.2** Modo híbrido local-first con TCP fallback
- [ ] **3.3** Configuración persistente via NVS (ESP32 Preferences)
- [ ] **3.4** Plugin Ircama para datos dinámicos
- [ ] **3.5** Scenario replay (grabar/reproducir sesiones interceptadas)

### Pendiente: Fase 4 — Avanzado

- [ ] **4.1** FreeRTOS — arquitectura no-bloqueante
- [ ] **4.2** CI pipeline (arduino-cli + cppcheck)
- [ ] **4.3** Web dashboard en ESP32
- [ ] **4.4** OTA firmware updates
- [ ] **4.5** WebSocket bridge + Docker Compose
- [ ] **4.6** Interfaz CAN bus física (ver `docs/FUTURE_CAN_BUS.md`)

## Decisiones de Arquitectura

- **Hardware**: ESP32-WROOM-32 (único ESP32 con BT Classic + BLE)
- **Protocolos OBD**: No implementados en ESP32 — Ircama maneja toda la simulación
- **AT Commands**: ~60 comandos divididos en locales/forward/pass-through
- **Identificación**: Respuestas locales llevan sufijo `[EMU]` para distinguir de Ircama

## Referencias

- [Ircama/ELM327-emulator](https://github.com/Ircama/ELM327-emulator)
- [ELM327 Datasheet](https://www.elmelectronics.com/wp-content/uploads/2016/07/ELM327DS.pdf)
- [jimwhitelaw/ELMulator](https://github.com/jimwhitelaw/ELMulator)
- [Freematics OBD-II Emulator MK2](https://freematics.com/pages/products/freematics-obd-emulator-mk2/)
