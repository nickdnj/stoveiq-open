# StoveIQ -- Open Source Smart Cooking Monitor

[![CI](https://github.com/nickdnj/stoveiq/actions/workflows/ci.yml/badge.svg)](https://github.com/nickdnj/stoveiq/actions)
[![License: MIT](https://img.shields.io/badge/Software-MIT-blue.svg)](LICENSE-SOFTWARE)
[![License: CERN-OHL-S-2.0](https://img.shields.io/badge/Hardware-CERN--OHL--S--2.0-orange.svg)](LICENSE-HARDWARE)

An ESP32-S3 + MLX90640 thermal camera that turns any stove into a smart cooktop. Real-time heatmap, per-burner temperature tracking, boil detection, smoke point warnings, and cook session logging -- all running locally with zero cloud dependency.

**No cloud. No subscriptions. No app to install. Just open your browser.**

> **Follow the build:** [YouTube Build Series](https://youtube.com/@vistter) | [Hackaday.io Build Log](https://hackaday.io/) *(project page coming soon)* | [GitHub Source](https://github.com/nickdnj/stoveiq)

## Features

- **Real-time thermal heatmap** -- 768-pixel IR view of your entire cooktop at 4Hz
- **Per-burner tracking** -- Auto-detects up to 4 burner zones with temperature, state, and trends
- **Smart alerts** -- Boil detection, oil smoke point warnings, pan preheated, forgotten burner
- **Cook session logging** -- Record and export thermal data from your cooking sessions
- **100% local** -- All processing on-device, data never leaves your kitchen
- **Works on any stove** -- Gas, electric coil, glass ceramic, induction

## Build Your Own

### Parts List (~$100)

| Part | Price | Source |
|------|-------|--------|
| ESP32-S3-DevKitC-1-N8R8 | ~$16 | Adafruit (PID 5312) |
| MLX90640 IR Camera Breakout 110deg FoV | ~$75 | Adafruit (PID 4469) |
| Jumper wires (4x F-F) | ~$1 | Any supplier |
| USB-C cable + 5V adapter | ~$3 | Any supplier |
| 3D printed enclosure | ~free | STL files in `/enclosure` |

### Wiring

```
ESP32-S3 DevKit     MLX90640 Breakout
--------------      ----------------
3V3          -----> VIN
GND          -----> GND
GPIO 1 (SDA) -----> SDA
GPIO 2 (SCL) -----> SCL
```

### Flash Firmware

```bash
# Install PlatformIO
pip install platformio

# Clone and build
git clone https://github.com/nickdnj/stoveiq.git
cd stoveiq-open/firmware
pio run -t upload

# Upload web UI (optional -- fallback UI is built into firmware)
pio run -t uploadfs
```

### Connect

1. Power on the device via USB-C
2. Connect to **StoveIQ** WiFi network (open, no password)
3. Open **http://192.168.4.1** in your browser
4. See your stove in thermal vision

To connect to your home WiFi (for access from any device):
- Use the **ESP BLE Prov** app (iOS/Android) with PoP: `stoveiq-setup`
- Or configure via the web UI settings page

## How It Works

```
MLX90640 IR Camera (24x32 pixels, 4Hz)
        |
        v
ESP32-S3 (FreeRTOS)
  Task 1: Sensor Read (Core 1)    -- I2C frame acquisition
  Task 2: Cooking Engine (Core 0) -- CCL burner detection + alerts
  Task 3: Web Server (Core 0)     -- HTTP + WebSocket streaming
        |
        v
Browser (any device on WiFi)
  WebSocket binary frames --> Canvas 2D heatmap
  WebSocket JSON status   --> Per-burner dashboard
```

### Burner Detection

Connected Component Labeling (CCL) on thresholded thermal image:
1. Binary mask: pixels > ambient + 30C
2. Two-pass CCL with 8-connectivity
3. Filter noise (< 4 pixels) and spurious regions (> 200 pixels)
4. Rank by total heat, track centroids across frames
5. Classify state: OFF / HEATING / STABLE / COOLING via dT/dt

## Development

### Desktop Emulator

Build and run without hardware using the thermal scene emulator:

```bash
cd firmware
pio run -e emulator
.pio/build/emulator/program
```

### Run Tests

```bash
cd firmware
pio test -e emulator
```

## Project Structure

```
stoveiq/
  firmware/
    src/              # Firmware source (C, ESP-IDF)
    include/          # Shared headers
    test/             # Unity test suite
    emulator/         # Desktop thermal emulator
    data/             # Web UI files (SPIFFS)
    lib/MLX90640/     # Melexis sensor driver
  enclosure/          # OpenSCAD 3D-printable enclosure
  docs/               # Build guide, BOM, wiring diagrams
```

## Contributing

See [CONTRIBUTING.md](docs/CONTRIBUTING.md) *(coming soon)* for development setup, testing, and code style guidelines.

Contributions welcome -- especially:
- Web UI improvements (the `firmware/data/` directory)
- New cooking alert types
- Enclosure variants for different mounting scenarios
- Translations

## License

- **Software:** [MIT](LICENSE-SOFTWARE)
- **Hardware:** [CERN-OHL-S-2.0](LICENSE-HARDWARE)

## Inspired By

- [Combustion Inc](https://combustion.inc/) -- Smart cooking thermometer with predictive algorithms (internal meat temps via probe). StoveIQ provides the complementary view: surface thermal imaging of the entire cooktop from above.

---

## Follow the Project

| Platform | Link | What You'll Find |
|----------|------|-----------------|
| **GitHub** | [nickdnj/stoveiq](https://github.com/nickdnj/stoveiq) | Source code, hardware files, issues |
| **YouTube** | [@vistter](https://youtube.com/@vistter) | Build videos, cooking demos, deep dives |
| **Hackaday.io** | *(coming soon)* | Build log, community discussion |

**Planned video series:**
1. "I Built an Open Source Smart Cooking Monitor" -- launch overview + demo
2. "Real-Time Thermal Imaging with ESP32" -- WebSocket streaming deep dive
3. "Can an IR Camera Detect When Water Boils?" -- cooking science + algorithm
4. "3D Printing a Parametric Enclosure in OpenSCAD" -- design + print process
