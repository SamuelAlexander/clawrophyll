"""Clawrophyll Kids -- Linux side.

Deliberately passive: all behaviour lives in the MCU sketch, so the demo works
even if this never runs. This only logs the sketch's status line every few
seconds, for tuning thresholds over `arduino-app-cli app logs`.
"""

import time

from arduino.app_utils import App, Bridge

LOG_EVERY_S = 10


def loop():
    time.sleep(LOG_EVERY_S)
    try:
        print(Bridge.call("read_status"), flush=True)
    except Exception as e:  # sketch still booting, or being reflashed
        print(f"status unavailable: {e}", flush=True)


App.run(user_loop=loop)
