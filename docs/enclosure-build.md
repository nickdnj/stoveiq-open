# StoveIQ PVC Pipe Enclosure — Build Guide

**Version:** 1.0  
**Date:** 2026-04-10  
**Estimated build time:** 45 minutes  
**Estimated cost:** $3-5 in materials (excluding components)

---

## Overview

This enclosure uses 1.5" Schedule 40 PVC pipe split lengthwise into two D-shaped halves. One half holds the MLX90640 thermal camera breakout. The other half holds the ESP32-S3-DevKitC-1. The halves snap together around the wiring and are held by zip ties. The assembled tube slides into a 3D-printed bracket that mounts under your kitchen cabinet, and the whole thing rotates to aim the camera at your stove before locking.

**Why PVC pipe?** It is at every hardware store, costs under $1 for a 6-inch section, is food-safe, heat-tolerant to 60°C continuous (fine for under-cabinet — the sensor does the hot work, not the housing), and the circular cross-section makes rotation-and-lock aiming trivial.

```
                  CABINET UNDERSIDE
    ┌─────────────────────────────────────┐
    │  [ screw ]  [ bracket plate ]  [ screw ] │ ← #8 wood screws into cabinet
    └──────────────┬──────────────────────┘
                   │ (bracket arms)
            ╔══════╧══════╗
            ║  ESP32 half ║  ← upper D-half (curved face against cabinet cradle)
            ╠═════════════╣  ← split line / flat face / alignment tabs
            ║ Sensor half ║  ← lower D-half
            ╚══════╤══════╝
                   │ (camera window here, 14mm hole)
                   ▼ ← MLX90640 looks DOWN at stove
                 stove
```

---

## Parts List

### Hardware Store (under $5)
| Item | Qty | Notes |
|------|-----|-------|
| 1.5" Schedule 40 PVC pipe | 1 section | Buy a 10" length; cut to 120mm (4.75") |
| Zip ties, 4" | 4 | To clamp halves together at groove positions |
| #8 x 1" flathead wood screws | 2 | For bracket-to-cabinet mount |
| M2 x 6mm pan head screws | 4 | To mount PCBs to standoffs (optional — friction fit works too) |

**PVC note:** 1.5" nominal = 1.900" OD = 48.26mm. This is Schedule 40, the standard white pipe at any hardware store. Do not use Schedule 80 (grey, thicker wall, smaller ID). Do not use 1.5" DWV (drain) pipe — it has a slightly different OD.

### 3D-Printed Parts
All files are in `hardware/enclosure/`. Print in PETG (indoor) or ASA (outdoor/near-stove heat). Layer height 0.2mm, 35% infill, 3 perimeters. No supports needed on any part when printed flat-face-down.

| File | Part | Print orientation | Qty |
|------|------|-------------------|-----|
| `stoveiq_pvc_sensor_half.step` | Sensor half shell | Flat face (D-face) down | 1 |
| `stoveiq_pvc_esp32_half.step` | ESP32 half shell | Flat face (D-face) down | 1 |
| `stoveiq_pvc_end_cap.step` | End cap | Flange face down | 2 |
| `stoveiq_pvc_bracket.step` | Cabinet bracket | Plate face up | 1 |

**Alternative — real PVC + printed inserts:** Buy a 6" PVC pipe nipple and cut it with a hacksaw. Then print only the alignment tab inserts, standoff blocks, and bracket. This saves print time and gives you a more authentic PVC look. See Section 5 for the hybrid approach.

### Electronics (already in hand)
- ESP32-S3-DevKitC-1 (Adafruit #5312)
- MLX90640 breakout 110° FoV (Adafruit #4469)
- 4x female-to-female jumper wires (I2C: SDA, SCL, 3.3V, GND)
- USB-C cable (for power)

### Tools
- Hacksaw or PVC cutter (if cutting real pipe)
- Hand drill + 9/16" bit (14mm) for camera window — or just use the 3D-printed sensor half
- Round file or sandpaper (to clean up the camera window cut)
- Hot glue gun (optional — to pot the MLX90640 board in place if not using screws)

---

## Step 1 — Pipe Preparation

**If using 3D-printed halves, skip to Step 2.**

If you are cutting a real 1.5" PVC pipe:

1. Mark a line around the pipe at **120mm (4.75")** from one end. Cut square with a hacksaw.
2. On the cut section, mark the centerline — a straight line running the full 120mm length on both sides. The easiest way: lay the pipe against a flat surface, hold a pencil flat on the surface, and roll the pipe so the pencil marks two parallel lines at the diameter.
3. Clamp the pipe securely (a vise with soft jaws, or wrap in a towel). Cut along the centerline on one side with a hacksaw. Rotate 180° and cut the other side. The two halves will separate.
4. Clean up the cut face with 120-grit sandpaper on a flat surface. The flat faces need to mate cleanly for the zip-tie clamping to work.
5. For the camera window: on the **sensor half** (the half that will face downward), mark the center at **30mm from the connector end** of the board. Drill a pilot hole with a 1/8" bit, then open up to 9/16" (14mm). File smooth. The hole should be on the **curved outer face**, not the flat face.

---

## Step 2 — Mount the MLX90640 (Sensor Half)

1. Place the MLX90640 breakout board inside the **sensor half** (lower half). The camera lens should be centered over the 14mm window hole, facing outward through the pipe wall.

2. The board seats on four **3mm standoffs** molded into the inner flat face of the sensor half. The board rests flat-face-down, camera lens pointing through the curved wall.

3. **Lens alignment check:** Before fastening, look through the camera window from outside. You should see the gold TO-39 sensor dome centered in the hole with clear space around it. The 110° FoV requires no obstruction within ~7mm of the lens face.

4. Secure with M2 x 6mm screws into the standoff holes, or apply a small bead of hot glue around the PCB perimeter. Do not glue over the sensor dome or the I2C header pins.

5. Route the 4 jumper wires (SDA, SCL, 3.3V, GND) out the **open end** of the sensor half (the end opposite the camera window). Leave 100mm of wire length — plenty to reach through the assembled tube.

**MLX90640 pinout (Adafruit 4469 breakout):**
| Pin | Color (standard jumper) | Connects to |
|-----|------------------------|-------------|
| VIN | Red | ESP32 3V3 pin |
| GND | Black | ESP32 GND |
| SDA | Blue | ESP32 GPIO8 (default I2C SDA) |
| SCL | Yellow | ESP32 GPIO9 (default I2C SCL) |

---

## Step 3 — Mount the ESP32-S3-DevKitC-1 (ESP32 Half)

1. Place the ESP32-S3-DevKitC-1 inside the **ESP32 half** (upper half). The USB-C port faces the **notch end** of the half — the 13mm wide x 8mm tall slot cut into the end wall. The USB-C cable exits through this notch.

2. The board seats on four standoffs in the upper half, same style as the sensor half. The component side (antenna, USB-C) faces upward into the curved shell. The flat underside of the board faces down toward the split line.

3. Secure with M2 x 6mm screws or hot glue on the board edges.

4. Verify the USB-C connector is accessible through the notch and a cable can be plugged in comfortably with the half assembled. The notch is 13mm wide — most USB-C cables fit. If your cable is bulky, file the notch slightly wider.

---

## Step 4 — Connect and Test Before Closing

**Do not close the enclosure until you confirm thermal data is flowing.**

1. Connect the 4 jumper wires from the MLX90640 to the matching ESP32 header pins.

2. Plug USB-C into the ESP32 and power it up.

3. Connect to the StoveIQ web dashboard (http://stoveiq.local or the assigned IP). You should see the 32x24 thermal heatmap populated with room-temperature data within 5 seconds.

4. Wave your hand over the MLX90640 sensor. You should see a warm blob track across the heatmap. If the frame is all zeros or all 255s, check your SDA/SCL connections — they may be swapped.

5. If everything works, power down before closing.

---

## Step 5 — Close and Secure the Halves

1. Lay the **sensor half** flat-face-up on your work surface (camera window facing down).

2. Lay the jumper wires in the channel between the two boards — there is roughly 8mm of space in the center of the tube.

3. Lower the **ESP32 half** onto the sensor half. The **alignment tabs** (small rectangular protrusions on the ESP32 half flat face) should drop into the **slots** in the sensor half flat face. They are located 20mm and 100mm from the notch end. You will feel a slight click as they seat.

4. The two halves should mate flush on both flat faces with no gap. If there is a gap, check that no wires are pinched at the split line.

5. Slide two zip ties through the **circumferential grooves** on the OD (at 15mm and 105mm from the USB end). Pull snug — not overtight. The zip tie sits in the groove and prevents the halves from springing apart. Trim the tails flush.

---

## Step 6 — End Caps

The end caps are optional. They keep dust out and improve the appearance.

1. Press the end cap spigot into each open end of the assembled tube. The spigot is sized for a light press fit (0.4mm undersize). Push in by hand; if it resists, sand the spigot lightly.

2. The cable-exit end (USB-C side) has a 7mm vent hole in the center for the USB-C cable to pass through. Thread the cable before pressing the cap in.

3. The opposite end has a solid cap (or also vented if you want future cable routing flexibility).

---

## Step 7 — Cabinet Bracket Installation

The bracket is a flat plate with two cabinet screw holes and a U-shaped cradle that holds the pipe.

### Bracket dimensions
- Plate: 62mm x 42mm x 3mm
- Cradle wrap: 220° arc around pipe OD
- Pipe clearance in cradle: 0.6mm (rotates freely before clamping)
- Clamp screw: M3 x 12mm through one cradle arm

### Installation

1. Hold the bracket plate flat against the cabinet underside at your desired location. The camera window should be roughly centered over the stove when the tube is horizontal.

2. Mark and pre-drill two pilot holes for #8 wood screws. The holes are 40mm apart centered on the plate.

3. Drive #8 x 1" flathead screws. The plate has countersinks for a flush mount.

4. Slide the assembled tube through the bracket cradle from the side. The cradle is open at the bottom — the tube snaps up in.

5. **Aim the camera:** Rotate the tube to point the sensor window directly downward at the stove. For a standard counter height of 36" and the MLX90640's 110° horizontal FoV, mounting the camera 18" above the stove surface covers a 36" wide field — enough for a standard 4-burner range.

6. Once aimed, tighten the M3 x 12mm clamp screw through the cradle arm. This squeezes the cradle slightly and locks the rotation angle. Do not overtighten — finger tight plus a quarter turn is enough.

---

## Aiming Reference

| Mount height above stove | FoV coverage at stove surface |
|--------------------------|-------------------------------|
| 12 inches (30cm) | 24" wide x 16" deep |
| 18 inches (46cm) | 36" wide x 24" deep |
| 24 inches (61cm) | 48" wide x 32" deep |
| 30 inches (76cm) | 60" wide x 40" deep |

*Based on MLX90640 BAB variant: 110° H x 75° V FoV. FoV width = 2 x height x tan(55°). Assumes camera pointing straight down.*

The MLX90640 outputs a 32 x 24 pixel thermal array. At 18" mount height covering a 36" wide stove, each pixel covers approximately 1.1" x 1.3" — more than sufficient to locate individual burners.

---

## Hybrid Build Option (Real PVC + Printed Inserts Only)

For a more rugged build that still uses real PVC pipe:

1. Buy a 6" section of 1.5" Sch 40 PVC and cut it as described in Step 1.
2. Print only:
   - `stoveiq_pvc_bracket.step` (bracket — must be printed)
   - Two small standoff inserts (add to the .scad as a separate module if desired)
3. Drill the camera window with a 9/16" bit as described.
4. Glue the standoff blocks to the inside flat faces with PVC primer + ABS cement (or epoxy).
5. Everything else is the same.

This approach costs even less and the PVC pipe is already UV-stabilized and food-safe.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Heatmap all zeros | I2C not communicating | Check SDA/SCL pin assignment in firmware (GPIO8/9); verify 3.3V not 5V |
| Heatmap frozen / no update | MLX90640 in broken-pixel mode | Power cycle; check I2C pullups (4.7k to 3.3V) |
| USB-C cable won't fit in notch | Cable has oversized plug | File notch to 15mm wide, or use a right-angle USB-C adapter |
| Halves won't close flush | Wire pinched at split line | Route wires through center of tube, not along the split |
| Camera window obstructing FoV | Window hole off-center | Widen hole to 16mm with a round file; still clears the 14.5mm TO-39 body |
| Bracket won't grip cabinet (soft material) | Screw holes stripping | Use 1.5" screws instead of 1"; add a 1/8" plywood backer plate |

---

## Files

All mechanical source files in `hardware/enclosure/`:

| File | Format | Purpose |
|------|--------|---------|
| `stoveiq_pvc_enclosure.scad` | OpenSCAD | Parametric source — edit all dimensions here |
| `stoveiq_pvc_enclosure.FCMacro` | FreeCAD Python | FreeCAD macro — generates STEP files |
| `stoveiq_pvc_sensor_half.step` | STEP AP214 | Sensor half — print or CNC |
| `stoveiq_pvc_esp32_half.step` | STEP AP214 | ESP32 half |
| `stoveiq_pvc_end_cap.step` | STEP AP214 | End cap (print x2) |
| `stoveiq_pvc_bracket.step` | STEP AP214 | Cabinet mounting bracket |

To regenerate STEP files from OpenSCAD: open each `.scad`, set the desired part flag to `true`, render (F6), and export as STL. For STEP, run the FreeCAD macro.

---

## License

Hardware design files: CERN Open Hardware Licence Version 2 - Permissive (CERN-OHL-P v2).  
You are free to use, modify, and manufacture this design for any purpose including commercial, provided attribution is maintained.
