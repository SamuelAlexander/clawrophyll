# sketch/ -- STM32U585 firmware (Zephyr)

The MCU side of Clawrophyll. Owns all real-time and safety-critical hardware:

- I2C sensors (Qwiic, MCU bus): Modulino Thermo `0x44`, Light `0x53`, Distance `0x52`
- Analog: capacitive soil moisture sensor on A0 (AREF→3V3), linearly calibrated
- Actuators: SG90 servo arm (D3)
- GC9A01 1.28" round TFT -- the animated face (Adafruit_GC9A01A over hardware SPI)

## Protocol

Arduino App Lab **Bridge RPC** -- the UNO Q's native MPU<->MCU mechanism. The sketch
registers handlers with `Bridge.provide_safe(name, fn)` (dispatched from `loop()`, so
they can touch hardware safely); the Linux side -- the MCP server in `python/main.py` --
calls them with `Bridge.call(name, ...)` (synchronous, returns the value).

Registered handlers:
- `read_status()` -> sensors + current expression + arm gesture  *(done)*
- `set_expression(name)` -> apply one of ten face states  *(done)*
- `wave_arm(gesture)` -> play one of four arm gestures: wave / nudge / slump / idle  *(done)*

Why Bridge RPC over a hand-rolled serial JSON protocol: it is the native UNO Q transport,
the App Lab toolchain understands it, registration is a one-liner per function, and we
avoid reinventing framing/parsing on both sides.

## The servo arm (Phase 2c)

`wave_arm` runs a **non-blocking, eased** gesture engine: each gesture is a list of
waypoints, and a state machine in `loop()` nudges the servo a few degrees per 20 ms servo
frame toward each one, so the face animation and sensor polling keep running while the arm
moves. The servo attaches only while a gesture plays, then detaches (limp, silent, no idle
current). Each gesture has its own speed. Notes:

- The official `Servo` lib supports the `zephyr` arch and bit-bangs the pulse from a timer
  ISR, so any digital pin works (D3) and the 50 Hz signal holds in the background.
- The 50 Hz servo frame is the smoothness ceiling; blinks are suppressed during a gesture
  so the blink's blocking redraw can't hitch the motion.
- `kArmMirror` flips the swing direction (CW<->CCW) about rest for left/right-arm mounting;
  the safe angle clamp is symmetric so a flipped gesture reaches the same extent.
- Powered from board 5 V with a 100 µF cap at the servo connector (no brownout on hard moves).

Tuning knobs (top of `sketch.ino`): `kArmRest`, the safe clamp `kArmMin`/`kArmMax`, the
per-gesture waypoint tables, and each gesture's `step_deg` (speed). Re-tune once the arm is
mechanically mounted in the planter.

## Firmware-enforced safety (not overridable from Linux)

- Servo: `arm_angle()` clamps every commanded angle to a safe range regardless of the
  gesture, so the agent can't drive the arm past safe limits.

The intelligence can be creative; the firmware is not -- the limit holds even if the agent
misbehaves. The same pattern would gate a higher-stakes actuator (a self-watering pump: max
on-time per command, minimum interval between runs) in a future or industrial scale-up.

## Build / flash

Built and flashed through Arduino App Lab (`arduino-app-cli`), driven from the Mac by
`scripts/redeploy.sh`. Library pins live in `sketch.yaml` (explicit versions; transitive
deps listed too, since the CLI doesn't resolve them). The build/flash gotchas are in the
[root README's troubleshooting table](../README.md#troubleshooting).

## Status

Complete: sensors, the animated face, and the servo arm are all done and tuned on
hardware. Self-watering was considered and cancelled -- Phil senses soil moisture and
asks to be watered rather than pumping water himself (the human stays in the loop).
Vision, the sense->decide loop, and the wake word live on the Linux side
([`../python/main.py`](../python/main.py)); the full story is in the
[root README](../README.md).
