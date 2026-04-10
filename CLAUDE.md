# CLAUDE.md -- StoveIQ Open Source

## Project Overview

Open-source smart cooking monitor using ESP32-S3 + MLX90640 thermal camera.
Local-first architecture: device serves its own web UI over WiFi, no cloud.

## Architecture

3-task FreeRTOS pipeline:
- **Sensor Read** (Core 1, Pri 20): MLX90640 I2C at 4Hz
- **Cooking Engine** (Core 0, Pri 15): CCL burner detection + alerts
- **Web Server** (Core 0, Pri 10): HTTP + WebSocket streaming

Dual build: ESP32 (FreeRTOS) and native emulator (pthreads).

## Key Files

| File | Purpose |
|------|---------|
| `firmware/include/stoveiq_types.h` | All shared types (burner, alert, config) |
| `firmware/src/sensor.c/h` | MLX90640 driver + emulator abstraction |
| `firmware/src/cooking_engine.c/h` | CCL burner detection + alert logic |
| `firmware/src/web_server.c/h` | HTTP server + WebSocket + SPIFFS |
| `firmware/src/wifi_manager.c/h` | AP+STA WiFi + mDNS |
| `firmware/src/ble_provision.c/h` | BLE WiFi provisioning |
| `firmware/src/tasks.c/h` | FreeRTOS/pthread task orchestration |
| `firmware/src/main.c` | Entry points (ESP32 + emulator) |
| `firmware/data/` | Web UI files (served from SPIFFS) |
| `enclosure/` | OpenSCAD 3D-printable enclosure |

## Build Commands

```bash
# ESP32-S3 build
cd firmware && pio run

# Native emulator build + run
cd firmware && pio run -e emulator && .pio/build/emulator/program

# Run tests
cd firmware && pio test -e emulator

# Upload to device
cd firmware && pio run -t upload

# Upload web UI files to SPIFFS
cd firmware && pio run -t uploadfs
```

## WiFi Setup

Two modes for getting WiFi credentials onto the device:
1. **BLE Provisioning**: Use ESP BLE Prov app, PoP: `stoveiq-setup`
2. **Web UI**: Configure via settings page at 192.168.4.1

Device always creates AP ("StoveIQ") for direct access.

## Conventions

- C code follows ESP-IDF conventions (snake_case, `s_` for statics)
- All modules are platform-independent (ESP32 + native build)
- Types in `stoveiq_types.h`, logic in modules, tasks wire them together
- Web UI is vanilla JS (no frameworks), served from SPIFFS
- Tests use Unity framework
