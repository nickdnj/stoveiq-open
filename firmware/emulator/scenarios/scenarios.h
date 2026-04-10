/**
 * scenarios.h — Extern declarations for all built-in test scenarios
 */

#ifndef SCENARIOS_H
#define SCENARIOS_H

#include "thermal_emulator.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const emu_scenario_t scenario_single_burner_30min;
extern const emu_scenario_t scenario_all_burners;
extern const emu_scenario_t scenario_oven_interference;
extern const emu_scenario_t scenario_false_positive_sun;

#ifdef __cplusplus
}
#endif

#endif /* SCENARIOS_H */
