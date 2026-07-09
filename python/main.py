"""Clawrophyll -- the App Lab Python app: everything Phil runs on the Linux side.

Four subsystems, each a daemon thread under App.run() (see __main__ at the bottom):

1. The phil-hardware MCP server (HTTP/SSE on port 8765, declared in app.yaml).
   Exposes Phil's hardware to nanobot as MCP tools -- read_sensors,
   set_expression, wave_arm call into the MCU over the App Lab Bridge;
   take_photo_and_describe is Linux-side (webcam + Cloud LLM brick).
2. A boot-time face reflex: one sensor read at app start so Phil's face reflects
   his state within seconds of a power-cycle, not after the first heartbeat.
3. The "Clawrophyll" wake word: an Edge Impulse .eim listening continuously to
   the USB mic; on detect, an instant wave plus a spoken/Telegram status check.
4. Local TTS (Piper) so the status check is spoken aloud through the USB speaker.

Architecture:

    nanobot --HTTP MCP--> this server --Bridge.call--> MCU sketch
"""

from __future__ import annotations

import json
import os
import threading
import time

from arduino.app_utils import App, Bridge
from mcp.server.fastmcp import FastMCP

# Bind to 0.0.0.0 inside the container; app.yaml maps the port to host loopback.
mcp = FastMCP("phil-hardware", host="0.0.0.0", port=8765)


@mcp.tool()
def read_sensors() -> dict:
    """Read every sensor wired to the MCU plus Phil's current display state.
    Returns a dict with:
      air_temp_c, humidity_pct              -- Modulino Thermo
      light_lux, light_raw                  -- Modulino Light
      distance_mm                           -- Modulino Distance (ToF; null = no target)
      soil_moisture_pct, soil_moisture_raw  -- capacitive sensor on A0
      expression, arm_gesture
      thermo_ok, light_ok, distance_ok, display_ok  -- per-peripheral health flags
    """
    raw = Bridge.call("read_status")
    return json.loads(raw)


# Valid expressions, mirrored from the MCU firmware's set_expression().
EXPRESSIONS = (
    "thirsty", "sweating", "freezing", "too_wet", "too_dark",
    "too_bright", "happy", "sleepy", "angry", "idle",
)


@mcp.tool()
def set_expression(expression: str) -> dict:
    """Set Phil's face. `expression` must be one of: thirsty, sweating, freezing,
    too_wet, too_dark, too_bright, happy, sleepy, angry, idle. Returns ok=false
    if the name is unknown (the face is left unchanged)."""
    if expression not in EXPRESSIONS:
        return {"ok": False, "error": f"unknown expression {expression!r}",
                "valid": list(EXPRESSIONS)}
    ok = bool(Bridge.call("set_expression", expression))
    return {"ok": ok, "expression": expression}


# Valid gestures, mirrored from the MCU firmware's wave_arm().
GESTURES = ("wave", "nudge", "slump", "idle")


@mcp.tool()
def wave_arm(gesture: str) -> dict:
    """Move Phil's servo arm. `gesture` must be one of: wave (greet), nudge (get
    attention), slump (droop, miserable), idle (return to rest and relax). The
    motion plays out on the MCU over ~1-2s and this returns as soon as the gesture
    is accepted -- not when it finishes; a new gesture interrupts one in progress.
    Returns ok=false if the name is unknown (the arm doesn't move)."""
    if gesture not in GESTURES:
        return {"ok": False, "error": f"unknown gesture {gesture!r}",
                "valid": list(GESTURES)}
    ok = bool(Bridge.call("wave_arm", gesture))
    return {"ok": ok, "gesture": gesture}


# --- Vision: webcam capture + Claude description -----------------------------
#
# The webcam is a LINUX-side resource: capture happens here in the container via
# the App Lab Camera peripheral, NOT over the Bridge/MCU (unlike the other tools).
# We open the camera on demand and release it immediately -- Phil's "eye" opens
# only to look (privacy; no idle camera). The frame is described by the App Lab
# Cloud LLM brick (Claude, multimodal). The tool returns *text*: nanobot delivers
# MCP tool output to the model as text only -- it can't "see" a returned image --
# so Claude describes the frame and Phil gets the words back.

# The capture node is resolved at capture time (see _resolve_camera_source) -- do
# NOT hardcode /dev/videoN, it reorders when the camera changes (swapping the C270
# for a C922 moved the camera from video2 to video0). Set PHIL_CAM_SOURCE to override.
_CAM_RESOLUTION = (640, 480)        # plenty for scene description; cheap on tokens
_CAM_WARMUP_FRAMES = 8              # throwaway frames so auto-exposure/white-balance settle
_LAST_VIEW_PATH = "/tmp/phil_last_view.jpg"   # latest frame, for debugging / a future share

# Capture + describe is slow, single-owner (the camera), and infrequent -- serialise
# the whole operation so two tool calls can't race on the camera or the LLM client.
_vision_lock = threading.Lock()

_VISION_PROMPT = (
    "You are the eyes of Phil, a small potted houseplant on a desk. Look at this "
    "webcam frame from Phil's point of view and briefly say what you see: whether a "
    "person is present and what they seem to be doing, the room and the lighting, and "
    "anything notable about the plant itself if it's in view. Two or three sentences, "
    "factual and concrete. Do not add personality, mood, or advice -- just describe."
)

# Built lazily on first use, then reused. .with_memory(0) keeps each describe call
# stateless: no growing image history across photos, no per-call client re-init.
_vision_llm = None
_vision_error: str | None = None


def _read_secret(*names: str) -> str:
    """Read a value from the board-side python/.phil_secrets (git-ignored AND
    rsync-excluded, so secrets never enter the repo). Matches any of `names` as a
    KEY=value line; falls back to the same-named environment variables."""
    here = os.path.dirname(os.path.abspath(__file__))
    wanted = set(names)
    for path in (os.path.join(here, ".phil_secrets"), "/app/python/.phil_secrets"):
        try:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    key, sep, val = line.partition("=")
                    if sep and key.strip() in wanted:
                        return val.strip().strip('"').strip("'")
        except OSError:
            continue
    for n in names:
        v = os.environ.get(n)
        if v:
            return v
    return ""


def _read_api_key() -> str:
    """Anthropic key for the vision call (Cloud LLM brick env name is API_KEY)."""
    return _read_secret("ANTHROPIC_API_KEY", "API_KEY")


def _resolve_camera_source() -> str:
    """Find the USB webcam's capture node. /dev/videoN reorders when devices
    change, so prefer the stable by-id capture symlink (index0 = capture,
    index1 = metadata); fall back to the first /dev/video* that isn't a platform
    codec (e.g. the Qualcomm Venus decoder/encoder). PHIL_CAM_SOURCE overrides."""
    import glob

    override = os.environ.get("PHIL_CAM_SOURCE")
    if override:
        return override
    by_id = sorted(glob.glob("/dev/v4l/by-id/*-video-index0"))
    if by_id:
        return by_id[0]
    for dev in sorted(glob.glob("/dev/video*")):
        try:
            name = open(f"/sys/class/video4linux/{os.path.basename(dev)}/name").read().lower()
        except OSError:
            name = ""
        if any(tag in name for tag in ("venus", "decoder", "encoder", "codec", "isp")):
            continue                      # skip platform video codecs, not cameras
        return dev
    return "/dev/video0"


def _capture_jpeg() -> bytes | None:
    """Open the webcam, let exposure settle over a few throwaway frames, grab one
    keeper as JPEG bytes, then release the camera. Returns None on failure."""
    import cv2
    from arduino.app_peripherals.camera import Camera

    frame = None
    cam = Camera(_resolve_camera_source(), resolution=_CAM_RESOLUTION, fps=10)
    cam.start()
    try:
        for _ in range(_CAM_WARMUP_FRAMES):
            frame = cam.capture()
    finally:
        cam.stop()
    if frame is None:
        return None
    ok, buf = cv2.imencode(".jpg", frame)             # frame is a BGR numpy array
    if not ok:
        return None
    jpeg = buf.tobytes()
    try:                                              # best-effort: keep the last frame for debugging
        with open(_LAST_VIEW_PATH, "wb") as f:
            f.write(jpeg)
    except OSError:
        pass
    return jpeg


def _get_vision_llm():
    """Lazily build the Cloud LLM (Claude, Sonnet) for captioning. Retries on
    every call until it succeeds, so adding the key doesn't require a restart."""
    global _vision_llm, _vision_error
    if _vision_llm is not None:
        return _vision_llm
    try:
        from arduino.app_bricks.cloud_llm import CloudLLM, CloudModel

        api_key = _read_api_key()
        if not api_key:
            _vision_error = ("no Anthropic key -- create python/.phil_secrets on the "
                             "board with ANTHROPIC_API_KEY=...")
            return None
        # ANTHROPIC_CLAUDE == claude-sonnet-4-5 (the brick's tested Sonnet); strong
        # vision, far cheaper/faster than Opus. One constant to bump if desired.
        _vision_llm = CloudLLM(model=CloudModel.ANTHROPIC_CLAUDE, api_key=api_key).with_memory(0)
        _vision_error = None
    except Exception as e:                            # surface the cause, don't crash the server
        _vision_error = f"{type(e).__name__}: {e}"
        return None
    return _vision_llm


@mcp.tool()
def take_photo_and_describe(focus: str = "") -> dict:
    """Capture a photo from Phil's USB webcam and return a short text description
    of what he sees -- whether a person is present and what they're doing, the
    room and the lighting, and the plant itself if it's in view. The webcam is on
    the Linux side; the camera opens only for this call (~1-2 s) then releases, so
    Phil isn't watching continuously. `focus` optionally steers what to look for
    (e.g. "is someone at the desk?"). Returns {ok: true, description} on success,
    or {ok: false, error, ...} if the camera or the vision model is unavailable."""
    with _vision_lock:
        jpeg = _capture_jpeg()
        if not jpeg:
            return {"ok": False, "error": "camera capture failed"}
        llm = _get_vision_llm()
        if llm is None:
            return {"ok": False, "captured": True,
                    "error": f"vision model unavailable ({_vision_error})"}
        prompt = _VISION_PROMPT
        if focus.strip():
            prompt += f"\n\nPay particular attention to: {focus.strip()}"
        try:
            description = llm.chat(message=prompt, images=[jpeg])
        except Exception as e:                        # network/model error -> report, don't crash
            return {"ok": False, "captured": True,
                    "error": f"vision call failed: {type(e).__name__}: {e}"}
    return {"ok": True, "description": (description or "").strip()}


# --- Boot-time face reflex --------------------------------------------------
#
# The agent's first sense-and-decide cycle runs on the heartbeat, which fires one
# full interval (an hour) AFTER nanobot starts and resets on every restart -- so
# after a power-cycle Phil would otherwise sit on a default face for up to an hour.
# This is a cheap, non-agent reflex: once the MCU is up, read the sensors once and
# set a coarse expression so Phil reflects his state within seconds of boot. No LLM,
# no Telegram, no journal -- the agent still owns the real decide/report cycle.

def _boot_expression(d: dict) -> str:
    """Coarse, conservative state for the boot face. Deliberately loose -- the
    sense-and-decide skill owns the real, plant-tuned thresholds; this only avoids a
    blank/default face in the gap before the first heartbeat."""
    soil = d.get("soil_moisture_pct")
    temp = d.get("air_temp_c")
    lux = d.get("light_lux")
    if isinstance(soil, (int, float)) and soil < 20:  return "thirsty"
    if isinstance(soil, (int, float)) and soil > 85:  return "too_wet"
    if isinstance(temp, (int, float)) and temp < 16:  return "freezing"
    if isinstance(temp, (int, float)) and temp > 34:  return "sweating"
    if isinstance(lux, (int, float)) and lux < 20:    return "too_dark"
    return "idle"


def _boot_face() -> None:
    """Wait for the MCU to come up after an app start, then set Phil's face once."""
    for _ in range(15):                      # MCU/Bridge may not be ready immediately
        try:
            data = json.loads(Bridge.call("read_status"))
        except Exception:
            time.sleep(2)
            continue
        expr = _boot_expression(data)
        try:
            Bridge.call("set_expression", expr)
            print(f"[boot_face] set expression: {expr}", flush=True)
        except Exception as e:               # best-effort; never crash the server
            print(f"[boot_face] set_expression failed: {e}", flush=True)
        return
    print("[boot_face] MCU not ready; left the sketch's default face", flush=True)


# --- Text to speech (Piper, local) ------------------------------------------
#
# Phil speaks his wake-word greeting aloud through a USB speaker (App Lab Speaker
# peripheral -> USB DAC). Synthesis is local and offline via Piper (onnxruntime,
# aarch64). The voice model lives on the board (python/models/, downloaded on first
# use, git-ignored + rsync-excluded). Swap _TTS_VOICE to retune the voice.
#
# Known issue: this cheap USB DAC briefly mutes its output when the playback stream
# opens, so the first word can be clipped. It's a hardware quirk (a different DAC, or
# the UNO Q media carrier's audio out, should avoid it); we don't work around it.

_TTS_VOICE = "en_US-ryan-medium"
_TTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")
_TTS_VOICE_URL = ("https://huggingface.co/rhasspy/piper-voices/resolve/main/"
                  "en/en_US/ryan/medium/en_US-ryan-medium.onnx")
_TTS_VOLUME_PCT = 100        # USB DAC hardware playback volume (it defaults to ~61%)
_TTS_GAIN = 0.75             # software level on top of the maxed mixer (1.0 = full scale)
_speak_lock = threading.Lock()
_piper_voice = None


def _max_speaker_volume() -> None:
    """Raise the USB DAC's hardware playback volume. The AB13X defaults to ~61%
    (-23 dB), which is too quiet, and ALSA mixer state can reset on reboot/replug, so
    we set it before playback. Best-effort: resolves the USB audio card by name."""
    try:
        import alsaaudio

        for idx, name in enumerate(alsaaudio.cards()):
            controls = alsaaudio.mixers(cardindex=idx)
            # The DAC is the USB-ish card that has a playback "PCM" control (the
            # webcam mic card only has a capture control).
            if "usb" in name.lower() or "audio" in name.lower() or "PCM" in controls:
                for ctl in ("PCM", "Speaker", "Master"):
                    if ctl in controls:
                        try:
                            alsaaudio.Mixer(ctl, cardindex=idx).setvolume(_TTS_VOLUME_PCT)
                        except Exception:
                            pass
    except Exception as e:
        print(f"[tts] volume set skipped: {e}", flush=True)


def _ensure_voice_files() -> str | None:
    """Path to the Piper .onnx, downloading the voice (+ its .json) to python/models/
    if missing. Returns None on failure."""
    onnx = os.path.join(_TTS_DIR, f"{_TTS_VOICE}.onnx")
    cfg = onnx + ".json"
    try:
        os.makedirs(_TTS_DIR, exist_ok=True)
        import urllib.request

        for url, dst in ((_TTS_VOICE_URL, onnx), (_TTS_VOICE_URL + ".json", cfg)):
            if not os.path.exists(dst):
                print(f"[tts] downloading {os.path.basename(dst)}", flush=True)
                urllib.request.urlretrieve(url, dst)
        return onnx if os.path.exists(onnx) and os.path.exists(cfg) else None
    except Exception as e:
        print(f"[tts] voice download failed: {e}", flush=True)
        return None


def _get_voice():
    """Lazily load the Piper voice (held for reuse). None if unavailable."""
    global _piper_voice
    if _piper_voice is not None:
        return _piper_voice
    onnx = _ensure_voice_files()
    if not onnx:
        return None
    try:
        from piper import PiperVoice

        _piper_voice = PiperVoice.load(onnx, config_path=onnx + ".json")
        print(f"[tts] voice ready: {_TTS_VOICE}", flush=True)
    except Exception as e:
        print(f"[tts] voice load failed: {e}", flush=True)
        return None
    return _piper_voice


def _render(text: str):
    """Synthesize `text` with Piper into padded, gain-adjusted int16 PCM. Returns
    (audio, sample_rate) or None if TTS is unavailable."""
    voice = _get_voice()
    if voice is None:
        return None
    try:
        import numpy as np

        pcm = bytearray()
        rate = 22050
        for chunk in voice.synthesize(text):
            rate = getattr(chunk, "sample_rate", rate)
            pcm += chunk.audio_int16_bytes
        audio = np.frombuffer(bytes(pcm), dtype=np.int16)
        if _TTS_GAIN != 1.0:                          # scale loudness (clamped to int16)
            audio = np.clip(audio.astype(np.float32) * _TTS_GAIN,
                            -32768, 32767).astype(np.int16)
        return audio, rate
    except Exception as e:
        print(f"[tts] render failed: {e}", flush=True)
        return None


def _play(audio, rate: int) -> None:
    """Play int16 mono PCM through the USB speaker (App Lab Speaker peripheral)."""
    try:
        from arduino.app_peripherals.speaker import Speaker

        _max_speaker_volume()
        sp = Speaker(device=Speaker.USB_SPEAKER_1, sample_rate=rate, channels=1)
        sp.start()
        try:
            sp.play_pcm(audio)
        finally:
            sp.stop()
    except Exception as e:
        print(f"[tts] play failed: {e}", flush=True)


def _speak(text: str) -> None:
    """Synthesize and play `text`, serialized so two lines never overlap."""
    with _speak_lock:
        r = _render(text)
        if r is not None:
            _play(*r)


# --- Wake word: "Clawrophyll" (Edge Impulse KWS) ----------------------------
#
# An always-on, fully on-device keyword spotter. A daemon thread captures the USB
# mic (App Lab Microphone peripheral, C922 = ALSA card 0) in overlapping 1 s
# windows and runs an Edge Impulse .eim model on the Linux/A53 side. When it hears
# "Clawrophyll", Phil waves instantly, then does a quick on-demand status check:
# reads his sensors, sets his face to match, and says a one-line status (Claude, in
# his voice) to Telegram and aloud. Edge ML triggers; the cloud model phrases it.

_KWS_MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "models", "clawrophyll-kws-linux-aarch64.eim")
_KWS_WINDOW = 16000          # samples the model expects (1 s @ 16 kHz)
_KWS_HOP_SEC = 0.25          # slide the window every 250 ms (overlapping)
_KWS_THRESHOLD = 0.55        # fire when P(clawrophyll) >= this (tune on hardware)
_KWS_CONFIRM = 1             # consecutive windows over threshold before firing (1 = snappiest)
_KWS_COOLDOWN_SEC = 3.0      # one utterance = one wake; ignore re-triggers this long
_KWS_LABEL = "clawrophyll"

def _post_telegram(text: str) -> None:
    """Best-effort: post `text` to Sam's Telegram. No-op unless the bot token + chat
    id are present in python/.phil_secrets (board-only)."""
    token = _read_secret("TELEGRAM_BOT_TOKEN")
    chat = _read_secret("TELEGRAM_CHAT_ID")
    if not token or not chat:
        return
    try:
        import urllib.parse
        import urllib.request

        body = urllib.parse.urlencode({"chat_id": chat, "text": text}).encode()
        urllib.request.urlopen(
            f"https://api.telegram.org/bot{token}/sendMessage", data=body, timeout=8)
    except Exception as e:                            # network/Telegram error -> ignore
        print(f"[kws] telegram post failed: {e}", flush=True)


def _status_fallback(d: dict, expr: str) -> str:
    """Plain factual status line, used if the LLM is unavailable."""
    bits = []
    soil = d.get("soil_moisture_pct")
    temp = d.get("air_temp_c")
    lux = d.get("light_lux")
    if soil is not None:
        bits.append(f"soil {soil}%")
    if isinstance(temp, (int, float)):
        bits.append(f"{temp:.0f}C")
    if lux is not None:
        bits.append(f"light {lux}")
    return (", ".join(bits) + f". Feeling {expr}.") if bits else f"Feeling {expr}."


def _status_line(d: dict, expr: str) -> str:
    """One short status line in Phil's voice from the current readings (Claude).
    Falls back to a plain factual line if the model is unavailable."""
    facts = _status_fallback(d, expr)
    llm = _get_vision_llm()                # same Claude client; a text-only call here
    if llm is None:
        return facts
    try:
        prompt = (
            "You are Phil, a deadpan, dry potted Japanese cedar on a desk. Someone just "
            "called your name. In ONE short first-person sentence, tell them how you are "
            f"right now, based on these readings: {facts}. No preamble, no lists, no emoji."
        )
        return (llm.chat(message=prompt) or "").strip() or facts
    except Exception as e:                  # network/model error -> plain line
        print(f"[kws] status LLM failed: {e}", flush=True)
        return facts


def _status_report() -> None:
    """The wake-word utility: read the sensors, set the face to Phil's actual state,
    and say a one-line status in his voice to Telegram and aloud."""
    try:
        data = json.loads(Bridge.call("read_status"))
    except Exception as e:
        print(f"[kws] status read failed: {e}", flush=True)
        data = {}
    expr = _boot_expression(data)
    try:
        Bridge.call("set_expression", expr)
    except Exception:
        pass
    line = _status_line(data, expr)
    _post_telegram(line)
    _speak(line)


def _on_wake(score: float) -> None:
    """React to the wake word: instant wave, then a quick status check (sense -> set
    the face -> say a one-line status in his voice, to Telegram and aloud)."""
    print(f"[kws] WAKE (score={score:.2f})", flush=True)
    try:
        Bridge.call("wave_arm", "wave")
    except Exception as e:
        print(f"[kws] wave failed: {e}", flush=True)
    # Off-thread so the listen loop keeps running while he senses, thinks, and talks.
    threading.Thread(target=_status_report, daemon=True).start()


def _kws_loop() -> None:
    """Listen for the wake word forever. Best-effort: if the model or mic isn't
    available, log why and bow out without touching the rest of the server."""
    try:
        import numpy as np
        from arduino.app_peripherals.microphone import Microphone
        from edge_impulse_linux.runner import ImpulseRunner
    except Exception as e:
        print(f"[kws] disabled (import failed): {e}", flush=True)
        return
    if not os.path.exists(_KWS_MODEL):
        print(f"[kws] disabled (model not found at {_KWS_MODEL})", flush=True)
        return
    try:
        os.chmod(_KWS_MODEL, 0o755)       # rsync drops the +x bit; the runner needs it
    except OSError:
        pass

    runner = ImpulseRunner(_KWS_MODEL)
    try:
        info = runner.init()
        labels = info["model_parameters"]["labels"]
        if _KWS_LABEL not in labels:
            print(f"[kws] disabled (label {_KWS_LABEL!r} not in {labels})", flush=True)
            return
    except Exception as e:
        print(f"[kws] disabled (model init failed): {e}", flush=True)
        return
    print(f"[kws] listening for '{_KWS_LABEL}' (labels={labels}, thr={_KWS_THRESHOLD})",
          flush=True)

    while True:                           # outer loop reopens the mic after an error
        mic = None
        try:
            mic = Microphone(device=Microphone.USB_MIC_1, sample_rate=Microphone.RATE_16K,
                             channels=Microphone.CHANNELS_MONO, format=np.int16)
            mic.start()
            buf = np.zeros(0, dtype=np.int16)
            hits = 0
            last_fire = 0.0
            while True:
                chunk = np.asarray(mic.record_pcm(_KWS_HOP_SEC)).flatten().astype(np.int16)
                buf = np.concatenate([buf, chunk])[-_KWS_WINDOW:]
                if buf.shape[0] < _KWS_WINDOW:
                    continue
                res = runner.classify(buf.astype(np.int32).tolist())
                score = res["result"]["classification"].get(_KWS_LABEL, 0.0)
                hits = hits + 1 if score >= _KWS_THRESHOLD else 0
                now = time.time()
                if hits >= _KWS_CONFIRM and (now - last_fire) >= _KWS_COOLDOWN_SEC:
                    last_fire, hits = now, 0
                    _on_wake(score)
        except Exception as e:
            print(f"[kws] capture error, retrying in 2s: {e}", flush=True)
            try:
                if mic is not None:
                    mic.stop()
            except Exception:
                pass
            time.sleep(2)


def _run_mcp() -> None:
    """Start the MCP HTTP server in a background thread so App.run() can take
    over the main thread to keep the App Lab framework alive."""
    mcp.run(transport="sse")


if __name__ == "__main__":
    threading.Thread(target=_run_mcp, daemon=True).start()
    threading.Thread(target=_boot_face, daemon=True).start()
    threading.Thread(target=_kws_loop, daemon=True).start()
    threading.Thread(target=_get_voice, daemon=True).start()     # pre-warm the TTS voice
    App.run()
