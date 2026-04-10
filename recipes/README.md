# StoveIQ Recipes

Community-driven thermal cooking recipes for the [StoveIQ](https://github.com/nickdnj/stoveiq-open) open-source cooking coach.

## What is this?

StoveIQ is a $50 thermal camera (MLX90640 + ESP32-S3) that mounts under your cabinet and watches your stovetop. It detects burner zones, tracks temperatures, and coaches you through recipes in real time — all from a web dashboard on your phone.

These recipes are **thermal state machines** — each step knows what temperature to look for, when to start timers, and when to tell you to act. No guesswork, no watching a pot boil.

## How recipes work

Each recipe is a JSON file with a sequence of steps. Each step has:

| Field | Description |
|-------|-------------|
| `desc` | What to show the user ("Waiting for boil...") |
| `target_temp` | Target temperature in Celsius |
| `trigger` | What advances to the next step |
| `timer_sec` | Timer duration (for `timer_done` trigger) |
| `coach_msg` | Audio-chime coaching message on completion |

### Triggers

| Trigger | What it detects |
|---------|----------------|
| `target` | Burner temp reaches `target_temp` |
| `boil` | Temp hits ~100°C and stabilizes |
| `simmer` | Temp in 85-98°C range |
| `food_drop` | Sudden temp drop >15°C (food hit the pan) |
| `timer_done` | Step timer expires |
| `confirm` | User taps a confirmation button (e.g., "Added oil") |
| `manual` | User taps "Next Step" |
| `temp_below` | Temp drops below `target_temp` |

## Contributing a recipe

1. Fork this repo
2. Create a JSON file in the appropriate category folder
3. Follow the [schema](schema.json)
4. Test with StoveIQ's simulation mode (Settings → Simulation Mode)
5. Submit a PR with:
   - What stove type you tested on (gas, electric, induction)
   - What cookware you used
   - Any notes on temperature tuning

### Tips for good recipes

- **Start with a cold-detection step** (`target_temp: 50`) so the recipe knows when you've turned on the heat
- **Use `confirm` triggers** for user actions (adding ingredients, stirring)
- **Be specific in coaching messages** — "FLIP NOW!" is better than "Continue"
- **Include the stove type** — gas and induction heat very differently
- **Test with simulation mode** before submitting

## Recipe categories

```
recipes/
├── basics/          ← Rice, pasta, eggs, potatoes
├── proteins/        ← Steak, chicken, fish, shrimp
├── vegetables/      ← Caramelized onions, stir-fry, roasted
├── sauces/          ← Roux, caramel, pan sauce, reductions
├── baking/          ← Stovetop baking techniques
└── community/       ← Everything else
```

## Schema

See [schema.json](schema.json) for the full JSON Schema specification.

## License

All recipes in this repository are released under [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) — public domain. Use them however you want.
