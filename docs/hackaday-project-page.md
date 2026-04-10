# StoveIQ — Hackaday.io Project Page Draft

## Title
**StoveIQ: Open-Source Thermal Cooking Coach**

## Tagline
A ~$100 IR thermal camera that watches your stovetop and coaches you through recipes in real time.

## One-liner
ESP32-S3 + MLX90640 thermal camera = a cooking coach that actually sees what's happening on your stove.

---

## Project Description

### What is it?

StoveIQ is an open-source smart cooking monitor that uses a 24x32 infrared thermal camera (MLX90640) to watch your stovetop in real time. It detects individual burners, tracks temperatures, and guides you through recipes with thermal-aware coaching — all from a web dashboard on your phone.

No cloud. No app store. No subscription. Just a $100 device and a browser.

### How does it work?

Mount the sensor under your cabinet, pointing down at the stove. It creates a live thermal heatmap of your entire cooking surface. The firmware runs a cooking engine that:

- **Detects burner zones** using Connected Component Labeling on the thermal image
- **Tracks per-burner state** — heating, stable, cooling — with temperature rate monitoring
- **Coaches you through recipes** — thermal state machines that know when water is boiling, when a pan is preheated, when you drop food in
- **Alerts you** — boil detected, smoke point warning, pan preheated, forgotten burner

### What makes it different?

Every cooking app is a dumb timer. StoveIQ actually **sees** what's happening:

| Feature | StoveIQ | Timer Apps | Probe Thermometers |
|---------|---------|------------|-------------------|
| Contactless | Yes | N/A | No (probe in food) |
| Multi-burner | 4 simultaneous | Manual | 1 at a time |
| Event detection | Boil, simmer, food drop | None | None |
| No cloud required | Yes | Mostly no | Varies |
| Open source recipes | Community JSON | No | No |

### The recipe system

Recipes are JSON state machines with thermal triggers. The community contributes recipes via GitHub PRs, each tuned with real thermal data. Current library:

- White Rice (boil detect → simmer → timer)
- Seared Steak (preheat → oil confirm → sear timer → flip → rest)
- Pasta (boil detect → confirm → timer)
- Fried Eggs (low heat target → butter confirm → timer)
- Boiled Potatoes (boil → simmer → fork test)
- Caramelized Onions (low heat → patience timer)

---

## BOM (~$50)

| Component | Source | Price |
|-----------|--------|-------|
| MLX90640 24x32 IR Thermal Camera (110° FoV) | Adafruit PID 4469 | $74.95 |
| ESP32-S3-DevKitC-1-N8 | Adafruit PID 5312 | $15.95 |
| Jumper wires (4x) | Any | ~$1 |
| PVC pipe enclosure (1" or larger) | Hardware store | ~$3 |
| **Total** | | **~$95** |

*Note: MLX90640 modules are available cheaper from AliExpress (~$25-35), which could bring the total closer to $50.*

---

## Build Instructions

### Hardware (15 minutes)

1. Cut PVC pipe in half lengthwise
2. Drill hole in one half for the MLX90640 sensor window
3. Mount ESP32-S3 in the other half
4. Wire 4 connections: VIN→3V3, GND→GND, SDA→GPIO1, SCL→GPIO2
5. Snap halves together — rotate tube to aim at stove
6. Mount under cabinet with adhesive or bracket

### Software (5 minutes)

1. Clone the repo: `git clone https://github.com/nickdnj/stoveiq-open`
2. Install PlatformIO
3. `cd firmware && pio run -e esp32s3 -t upload`
4. Open "ESP BLE Provisioning" app on phone
5. Connect to "StoveIQ", enter PoP: `stoveiq-setup`
6. Select your WiFi network
7. Open `http://stoveiq.local` on any device

### Calibrate (2 minutes)

1. Gear icon → Calibrate Burners
2. Tap on each burner location in the heatmap
3. Drag to position, +/- to resize
4. Save

### Cook

1. Tap Recipes → pick one
2. Follow the thermal-aware coaching
3. The app tells you what it sees and when to act

---

## Technical Stack

- **MCU:** ESP32-S3-WROOM-1-N8R8 (WiFi + BLE)
- **Sensor:** MLX90640 24x32 IR thermal array, 110° FoV, 4Hz
- **Firmware:** ESP-IDF 5.5 + FreeRTOS (C)
- **UI:** Single-page web app served from firmware (no SPIFFS needed)
- **Comms:** WebSocket binary frames (thermal data) + JSON (status/commands)
- **Recipes:** JSON state machines with thermal triggers
- **Enclosure:** Split PVC pipe (hackable, adjustable angle)

---

## Links

- **Firmware repo:** github.com/nickdnj/stoveiq-open
- **Recipe community:** github.com/nickdnj/stoveiq-open/recipes
- **Web dashboard:** stoveiq.local (when connected to same network)

---

## Tags

thermal-imaging, cooking, esp32, mlx90640, open-source, iot, kitchen, recipe, infrared, food

---

## Project Logs (planned)

1. **From safety product to cooking coach** — the pivot story (patent killed the safety angle)
2. **First light** — breadboard to live heatmap in one session
3. **The recipe engine** — how thermal state machines make cooking apps obsolete
4. **Community recipes** — building a GitHub-powered recipe ecosystem
5. **PVC pipe enclosure** — the $3 adjustable-angle mounting solution

---

## Photos/Screenshots needed

- [ ] Breadboard setup (ESP32 + MLX90640 + 4 wires)
- [ ] PVC enclosure (cut, drilled, assembled)
- [ ] Mounted under cabinet
- [ ] Dashboard screenshot — heatmap with burner overlays
- [ ] Dashboard screenshot — recipe in progress with progress bar
- [ ] Dashboard screenshot — calibration mode
- [ ] Thermal image of actual stove with burners on
- [ ] Phone on counter showing dashboard while cooking
