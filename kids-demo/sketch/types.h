// Types used in function signatures. They live in a header because the Arduino
// builder inserts auto-generated prototypes above the sketch's own declarations.
#pragma once
#include <stdint.h>

enum Face { F_IDLE, F_HAPPY, F_SLEEPY, F_ANGRY, F_THIRSTY, F_SWEATING, F_FREEZING,
            F_TOO_WET, F_TOO_BRIGHT, F_SURPRISED, F_LOVE, F_DIZZY, F_MUSIC, F_SQUINT,
            F_SNEEZE };

struct EyeP { uint16_t color; float h; float w; int yoff; };

// Sequence step: a step shows a face (or a sensor card) for ms, optionally starting a gesture.
struct Step { int8_t face; int8_t gest; int8_t card; uint16_t ms; };

// Arm gesture: waypoints (ease to angle, then hold) + average speed in deg per 20 ms.
struct Waypoint { int angle; int hold_ms; };
struct Gesture { const char* name; const Waypoint* wp; int len; int step_deg; };
