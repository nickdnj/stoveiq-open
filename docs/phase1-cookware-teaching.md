# StoveIQ Phase 1 — Cookware-Based Calibration + Teaching Mode

**Status:** PLANNING (2026-04-21)
**Scope:** All changes phone-side (embedded HTML/JS in `web_server.c`). Zero firmware logic changes. Current Wake Lock + HTTPS stack stays put.

---

## 1. Problem Statement

Current calibration is:
- One global emissivity (0.54) applied to every pixel
- Per-burner spatial ROI (position on the sensor)

This is wrong because:
- Emissivity depends on cookware material (cast iron 0.95, polished steel 0.16 — a 6× spread)
- Surface emissivity changes during a cook: empty metal → oil → food covering the surface → water dominating
- A single offset per burner can't track that

**Fix:** Split calibration into two axes — burner (spatial, fixed) and cookware (thermal, pickable). The unit of calibration is the pair. The intelligence of phase-aware adjustment is deferred to Phase 2+; Phase 1 just captures clean data to make that possible.

---

## 2. Mental Model

```
┌──────────┐     ┌──────────┐     ┌────────────────────────────┐
│  Burner  │  ×  │ Cookware │  =  │  Cook Session              │
│  (fixed) │     │ (picked) │     │  label + events + samples  │
└──────────┘     └──────────┘     └────────────────────────────┘
```

- **Burner** — spatial ROI on the sensor. Doesn't move.
- **Cookware** — material + friendly name + icon. Lives in a user-managed library.
- **Cook Session** — active (burner, cookware) pair plus a label ("eggs", "pasta boil") + phase events + temperature timeseries.
- **Learned offset** — stored per (burner × cookware) pair, applied on display.

The teaching data IS the session corpus. Every labeled cook becomes training material for future phase-aware intelligence.

---

## 3. Data Model

All persisted in `localStorage` on the phone under the following keys. Schema version `1`.

### `siq_cookware` — cookware library
```json
[
  {
    "id": "cw_1712670000_abc123",
    "name": "10\" non-stick egg pan",
    "icon": "🍳",
    "material": "non_stick",
    "notes": "Scanpan, only for eggs",
    "created": "2026-04-21T10:00:00Z"
  }
]
```

### `siq_burner_cookware` — what's currently on each burner
```json
{ "0": "cw_1712670000_abc123", "2": "cw_1712670500_def456" }
```

### `siq_calib_offsets` — per-pair learned offset in °C
```json
{ "0_cw_1712670000_abc123": -12.5, "2_cw_1712670500_def456": 3.2 }
```

### `siq_active_sessions` — in-progress sessions keyed by burner id
```json
{
  "0": {
    "id": "s_1712670100_0",
    "burner_id": 0,
    "cookware_id": "cw_...",
    "label": "eggs",
    "started": "2026-04-21T10:01:40Z",
    "events": [
      { "t_ms": 45000, "type": "oil_in", "at": "2026-04-21T10:02:25Z" },
      { "t_ms": 72000, "type": "food_in", "at": "..." }
    ],
    "samples": [
      { "t_ms": 500, "temp_c": 23.4, "state": 0 },
      { "t_ms": 1000, "temp_c": 28.1, "state": 1 }
    ]
  }
}
```

### `siq_sessions` — past sessions (cap 200, FIFO eviction)
Same shape as above plus `"ended": "..."`.

### Material enum (presets shown in Add Cookware form)
| id | Label | Default ε |
|---|---|---|
| `cast_iron` | Cast iron (seasoned) | 0.95 |
| `non_stick` | Non-stick / enameled | 0.90 |
| `ss_scuffed` | Stainless (scuffed) | 0.35 |
| `ss_polished` | Stainless (polished) | 0.16 |
| `aluminum_ox` | Aluminum (oxidized) | 0.30 |
| `aluminum_shiny` | Aluminum (shiny) | 0.08 |
| `copper` | Copper | 0.05 |
| `glass` | Glass / Pyrex | 0.88 |

Phase 1 doesn't actively *use* the emissivity value — it's metadata. The applied correction is the per-pair offset. Material hint seeds future auto-offset logic.

### Session labels (curated + free-text fallback)
`sauté · boil · sear · simmer · fry · reduce · melt · steam · eggs · water · other`

### Event types (phase markers)
`oil_in · food_in · water_added · lid_on · lid_off · stirred · rolling_boil · flipped · done · note`

The `note` type carries an optional free-text payload for anything that doesn't fit the preset list.

---

## 4. UI Design

### Navigation model

Two major views: **Dashboard** (home) and **Burner View** (drill-in).

```
┌─────────────── Dashboard ───────────────┐
│  [FL card]  [FR card]                   │
│  [RL card]  [RR card]                   │   Tap any card → Burner View
│  [Alerts]                               │
│  [⚙ Settings]  [C/F]  [wake] [conn]    │
└─────────────────────────────────────────┘
```

Each burner card (existing) gets one new element: a **cookware chip** at the top showing the assigned pan (or "＋ tap to set"). Active-session cards show a red recording pulse indicator. No other dashboard changes — cookware selection and the graph live in the Burner View.

### Burner View (NEW — full-screen modal)

```
┌─────────────── Front Right ─────┬─ × ─┐
│ [🍳 10" non-stick egg pan]  Change   │
│ Label: eggs                  Edit    │
│                                       │
│  ╭─ Temp (°C) ─────────────────────╮ │
│  │ 250                             │ │
│  │ 200       ●───────────          │ │
│  │ 150   ●──●         ╲____        │ │
│  │ 100 ●──                  ─●──  │ │   LIVE graph, 30s/60s/5m window
│  │  50●                           │ │   Annotated events shown as dots
│  ╰───────────────────────────────╯ │
│   0m  1m  2m  3m  4m                 │
│                                       │
│  Mark event:                          │
│  [💧Oil in] [🍅Food in] [🥤Water]    │
│  [🫧Rolling boil] [🔄Stirred]        │
│  [🎩Lid on] [🎩Lid off] [✓Done]     │
│  [📝Note...]                          │
│                                       │
│  [ End Session ]   [ Calibrate Boil ] │
└───────────────────────────────────────┘
```

- **Live graph** — canvas-based, rolling window (selectable 1m/5m/entire session). Current temp ticks at 2 Hz.
- **Tap on graph** when viewing a past session → drops an event marker at that timestamp → action sheet picks event type. Retroactive labeling.
- **Event buttons** — one-tap during cook; brief beep + visual confirmation.
- **End Session** — seals and moves to past sessions, clears burner cookware.
- **Calibrate Boil** (Phase 1.5 stretch, include button, stub handler) — kicks off the 100°C water-boil auto-calibration. Phase 1 just logs intent.

### Cookware Library (modal, from Settings)

```
┌──────── Cookware ───────┬─ × ─┐
│ 🍳 10" non-stick egg pan │  [Del]
│ 🥘 12" stainless saute  │  [Del]
│ 🫕 4qt stock pot         │  [Del]
│                               │
│ [＋ Add cookware]              │
└───────────────────────────────┘
```

Add/Edit form: name, icon (emoji picker later; text input for now), material dropdown, notes.

### Sessions Browser (modal, from Settings)

```
┌──────── Sessions (23) ───────┬─ × ─┐
│ 🍳 eggs · FR · egg pan · 6m        │
│    2026-04-21 10:01 · 720 samples  │
│ 🫕 pasta boil · FL · stock pot·14m │
│    2026-04-20 18:30 · 1680 samples │
│ ...                                 │
│                                     │
│ [Export backup]  [Import…]          │
└─────────────────────────────────────┘
```

Tap row → opens the Burner View in **replay mode** with the full graph + all events, editable.

### Header & Settings integration

- Header unchanged (keep Recipes, C/F, gear, wake dot, conn dot).
- Settings panel gains two new links at the top: **Cookware Library** and **Sessions & Backup**.

---

## 5. Export / Import

Single gzipped JSON file: `stoveiq-backup-YYYY-MM-DD.json`.

```json
{
  "version": 1,
  "exported_at": "2026-04-21T12:00:00Z",
  "cookware": [...],
  "calib_offsets": {...},
  "burner_cookware": {...},
  "sessions": [...]
}
```

- **Export:** `navigator.share({files:[File]})` → iOS share sheet → AirDrop / Files / Mail / iCloud.
- Fallback: download link if `share` unavailable (desktop testing).
- **Import:** `<input type=file>` → parse → confirm merge vs replace → commit to localStorage.
- No server, no account, no cloud.

---

## 6. Calibration Offset Application

Phase 1 approach (pragmatic, not physically correct but good enough):

1. Firmware keeps its current global emissivity (0.54). No firmware change.
2. Phone-side: for each burner, look up its assigned cookware, find `calib_offsets[burner_id + '_' + cw_id]`, add to displayed temp.
3. **Samples stored in sessions are RAW** (pre-offset) to preserve fidelity for later reinterpretation.
4. Calibration method stub: **boil-water single point**. UI button exists in Burner View; actual auto-detect logic is Phase 2. Phase 1 supports manual entry: "set offset for this pair" spinner in Cookware Library row.

Future (Phase 2+): multi-point calibration, phase-aware offsets, auto-detect rolling boil signature, emissivity inference.

---

## 7. Implementation Tasks

All edits are in `firmware/src/web_server.c` unless noted.

| # | Task | LOC est |
|---|---|---|
| 1 | CSS: chips, event rows, modals, graph container | 80 |
| 2 | HTML: cookware picker modal, cookware form, sessions modal, burner view modal | 120 |
| 3 | Settings panel: add Cookware + Sessions buttons | 10 |
| 4 | JS state: load/save cookware, offsets, burner_cookware, active_sessions, past_sessions | 40 |
| 5 | JS: cookware CRUD (add/edit/delete) | 80 |
| 6 | JS: session start/end, event marking, sample recording | 70 |
| 7 | JS: burner view modal (open, render, close, live graph) | 150 |
| 8 | JS: canvas temp graph (draw, scale, event dots, tap-to-annotate) | 180 |
| 9 | JS: sessions browser (list, tap-to-open-replay, delete) | 60 |
| 10 | JS: export via Share API + import via file picker | 60 |
| 11 | Hook renderCards: cookware chip, offset-adjusted temp, active-session indicator | 30 |
| 12 | Hook WS onmessage: call recordSample for active sessions | 10 |
| **Total** | | **~900 LOC** |

Web server HTML string grows from ~600 lines to ~1500. Still fits comfortably in flash (currently 42% of 3MB).

---

## 8. Build + Flash Plan

1. **First-time only:** `./scripts/gen-cert.sh` to generate the TLS cert + key (git-ignored)
2. Make changes in `firmware/src/web_server.c`
3. `pio run -e esp32s3` — verify compile
4. `pio run -e esp32s3 -t upload --upload-port /dev/cu.usbmodem1101` — flash
5. On phone: hard-quit Safari → reopen `https://stoveiq.local` → accept cert if first time
6. Bump service worker cache to `stoveiq-v3` to force fresh HTML fetch
7. Sanity: verify burners still render, WS still connects, add first cookware, start session, verify graph streams

---

## 9. What's NOT in Phase 1

- Auto-boil detection / emissivity inference
- Phase-aware offsets
- Cloud sync / multi-device sharing
- Voice annotation ("hey StoveIQ, water in")
- ML dish-signature library
- Firmware-side offset application
- Multi-user / household accounts
- Rolling-buffer LittleFS backup on the ESP32 (phone is sole long-term store in v1)

All listed for Phase 2+.

---

## 10. Open Questions

1. **Multi-burner concurrent sessions** — support N burners recording simultaneously? → YES, data model handles it; UI just needs per-burner state.
2. **Retroactive annotation on active sessions** — allow or require End Session first? → ALLOW, no reason to block.
3. **Event icon set** — emoji or SVG? → emoji (lighter, HTML-native, matches existing UI).
4. **Graph Y-axis** — auto-scale or fixed 0-300°C? → auto-scale with minimum 50°C floor and 30°C headroom.
5. **Session display duration** — show all or rolling window? → toggle: Live / 5m / All.

Answer on these locks scope. Otherwise: **ready to build**.
