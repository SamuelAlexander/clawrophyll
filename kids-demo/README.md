# Clawrophyll Kids

An offline, hands-on spin-off of [Clawrophyll](../README.md) for classroom demos. No AI
agent, no internet, no camera, mic, or speaker. The sensors are the controls: kids poke,
cover, shine, and breathe on them, and the plant answers instantly with a face on the round
display and a move of its pincer. Between pokes it stays lively on its own.

Same hardware and wiring as Clawrophyll (see the [BOM and wiring](../README.md)), minus the
USB camera, mic, and speaker. Any plant works; this build used a cactus.

## What the kids can do

| Kids do | Face + pincer | Then the display shows |
|---|---|---|
| Hand in front (12 to 40 cm) | happy + wave | distance card |
| Hand very close (under 12 cm) | surprised + slow, gentle nudge | distance card |
| Keep a hand there for 5 s | hearts + sway | |
| Jab a finger at the distance sensor (closer than 10 cm) 2 times in 10 s | angry "Hey!" | |
| Cover the light sensor (it faces up) | sleepy Zzz + slump | light card |
| Shine a phone torch on it | too bright + raises the claw for shade | light card |
| Breathe on the thermo sensor | sweating + fans himself | humidity card |
| Dip the soil probe in wet soil | too wet | soil card |
| Pull the probe out and wipe it dry | thirsty + slump | soil card |

Each card is a big live number with a gauge ring and a short caption ("So dark!",
"Your breath!"), so kids can see the sensor change as they play.

**When nobody is playing**, the face blinks now and then, and every 20 s to 2 min the plant
plays a random skit: wave hello, hearts, dizzy spiral eyes, a dance with music notes, a yawn
and stretch, a sneeze ("ah... ah... ACHOO!"), or a tour of all five sensor cards.

## How it works

Everything runs in the MCU sketch ([`sketch/sketch.ino`](sketch/sketch.ino)). The Linux
side ([`python/main.py`](python/main.py)) only logs the sketch's status every 10 s for
tuning; nothing depends on it.

- **Relative triggers.** Light, temperature, and humidity react to a change from a slowly
  adapting baseline, not fixed numbers, so a hot classroom or a dim room doesn't leave the
  plant stuck in one face. The soil probe and distance sensor use fixed thresholds.
- **Hysteresis and dwell.** Each condition has separate enter and exit thresholds and must
  hold briefly before it counts, so a wobbly reading can't flicker the face.
- **Long-held conditions settle down.** After 20 s the face returns to idle. After 60 s a
  light or temperature change is accepted as the new normal; a soil or hand condition
  instead replays its reaction once a minute.
- **The arm doesn't overdo it.** After a gesture the arm rests at least 12 s
  (`kArmCooldownMs`), and the same reaction within 20 s (`kRepeatMs`) shows only the face;
  faces and cards still react every time.
- **Smooth, slow arm.** Gestures are time-based with cosine easing, so screen redraws can't
  stall them. Every angle is clamped to a safe range in firmware, and moves near a very
  close hand are slower.

## Deploy and switch modes

From the repo root on the Mac (needs `UNOQ_HOST` in `.env`, as for Clawrophyll):

```bash
kids-demo/deploy.sh
```

Syncs this folder to `~/ArduinoApps/clawrophyll-kids`, builds and starts it, makes it the
board's boot app, and disables the agent's services (`clawrophyll-app`, `nanobot`) so the
board boots straight into the demo with no network.

```bash
kids-demo/deploy.sh agent
```

Switches back to the full Clawrophyll agent, at boot too.

```bash
kids-demo/deploy.sh logs
```

Follows the live status log (sensor values, baselines, active conditions, current face).

## Classroom checklist

- Unplug the USB camera, mic, and speaker. Only the stable power supply is needed.
- Power on 1 to 2 minutes before the kids arrive: the app needs about 50 s after boot.
- Keep hands off the sensors for the first few seconds after it starts, while it learns
  the room's normal light, temperature, and humidity.
- Hold a phone torch about 10 cm above the light sensor; the room's own light won't
  trigger "too bright".
- Have a small cup of wet soil ready for the probe, and a tissue to wipe it dry.

## Tuning

All knobs are constants at the top of [`sketch/sketch.ino`](sketch/sketch.ino) and in the
arm section:

| Constant | What it sets |
|---|---|
| `kSkitMinMs`, `kSkitMaxMs` | Random gap between idle skits (20 s to 2 min) |
| `kReactMs`, `kCardMs` | How long a reaction face and its sensor card show |
| `kStaleMs`, `kRebaseMs`, `kNagMs` | When a long-held condition settles down |
| `kLoveMs` | How long a hand must stay for hearts |
| `kSoilDry`, `kSoilWet` | Soil probe calibration (raw reading in air / in water) |
| `kArmCooldownMs`, `kRepeatMs` | Minimum arm rest between gestures; face-only repeats |
| `kArmSpeed` | Overall arm speed (1 = Clawrophyll's speed, now 0.2) |
| `kArmMin`, `kArmMax`, `kArmRest` | Safe servo range and rest angle |

Trigger thresholds live in `cond_want()`. Shared types live in
[`sketch/types.h`](sketch/types.h), because the Arduino builder inserts its generated
prototypes above any types declared in the `.ino`.
