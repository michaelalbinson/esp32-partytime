"""Server configuration: environment settings and device-tuning constants."""

import os

from dotenv import load_dotenv

load_dotenv()

# --- Environment ------------------------------------------------------------

HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "5050"))
DEBUG = os.getenv("DEBUG", "False").lower() in ("true", "1", "t")
MAX_GIF_BYTES = int(os.getenv("MAX_GIF_BYTES", str(5 * 1024 * 1024)))
MAX_FRAMES = int(os.getenv("MAX_FRAMES", "60"))
DEVICE_TIMEOUT_SECONDS = int(os.getenv("DEVICE_TIMEOUT_SECONDS", "10"))
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO")

# Where devices.json, shows.json and the gifs/ library live.
DATA_DIR = os.getenv("DATA_DIR", ".")

# --- GIF timing --------------------------------------------------------------

# Fallback per-frame delay when a GIF carries none (or an animation predates
# stored delays).
DEFAULT_FRAME_MS = 120

# Shortest frame delay we will use. GIF delays are in 10 ms units and many GIFs
# specify 0, which browsers bump to 100 ms; that looked choppy on the devices,
# so we only enforce this small floor instead.
MIN_GIF_DELAY_MS = 10

# --- Display geometry and frame budgets -----------------------------------------

# The ESP32 RGB332 frame buffer can hold at most 144 * 192 pixels.
ANIM_MAX_PIXELS = 144 * 192

# The ST7735 keeps the whole animation in RAM so playback doesn't wait on
# WiFi. Frames are 1 byte per pixel, so every frame's pixel count is capped at
# ANIM_CACHE_BYTES / frame_count. Keep this in step with CACHE_BUDGET_BYTES in
# sketch/esp-st7735. Many-frame GIFs stop shrinking at ANIM_MIN_PIXELS and the
# frames that then don't fit are streamed.
ANIM_CACHE_BYTES = 120 * 1024
ANIM_MIN_PIXELS = 64 * 64

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 320

# SSD1306 OLED: 128x64, 1 bit per pixel.
MONO_WIDTH = 128
MONO_HEIGHT = 64

# Many SSD1306 panels are two-colour: a yellow band across the top 16 rows and
# blue below. The top band is reserved for the dancing message text, so the GIF
# is fitted into the blue area underneath.
MONO_TEXT_BAND = 16

# --- Shows -----------------------------------------------------------------

# A device plays one program: its lane of a show. The firmware keeps the whole
# program in fixed-size arrays, so these limits mirror TL_MAX_* in
# sketch/*/timeline.h.
MAX_PROGRAM_FRAMES = MAX_FRAMES   # distinct frames across a lane's GIFs
MAX_PROGRAM_CLIPS = 16            # distinct GIFs in a lane
MAX_LANE_STEPS = 24

# Effects the devices generate themselves, at their full native resolution.
# The numbers are what the firmware receives; keep them in step with
# TL_EFFECT_* in sketch/*/timeline.h.
EFFECTS = {
    "rain": 1,
    "static": 2,
    "equalizer": 3,
    "scanner": 4,
    "radar": 5,
    "testpattern": 6,
    "snow": 7,
    "confetti": 8,
    "bounce": 9,
}

# Text steps show their own text in one of these styles, drawn by the device.
# Keep in step with TL_STYLE_* in sketch/*/timeline.h.
TEXT_STYLES = {"marquee": 1, "flash": 2, "wave": 3}
MAX_TEXT_LENGTH = 100   # characters; devices keep plain ASCII only
MAX_PROGRAM_TEXTS = 8   # distinct texts in one lane

MAX_LANES = 8
MIN_STEP_SECONDS = 0.1
MAX_STEP_SECONDS = 3600
MAX_NAME_LENGTH = 80
