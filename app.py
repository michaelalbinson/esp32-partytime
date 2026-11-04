from flask import Flask, jsonify, request, render_template, Response
from threading import Lock
import base64
import io
import os
import json
import logging
from datetime import datetime, timedelta

from PIL import Image, ImageSequence
from dotenv import load_dotenv

load_dotenv()

# Configuration
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "5050"))
DEBUG = os.getenv("DEBUG", "False").lower() in ("true", "1", "t")
MAX_GIF_BYTES = int(os.getenv("MAX_GIF_BYTES", str(5 * 1024 * 1024)))
MAX_FRAMES = int(os.getenv("MAX_FRAMES", "60"))
DEVICE_TIMEOUT_SECONDS = int(os.getenv("DEVICE_TIMEOUT_SECONDS", "10"))
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO")
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "85"))  # Retained for compatibility.

# The ESP32 RGB332 frame buffer can hold at most 144 * 192 pixels.
ANIM_MAX_PIXELS = 144 * 192

SCREEN_WIDTH = 240
SCREEN_HEIGHT = 320

# SSD1306 OLED: 128x64, 1 bit per pixel.
MONO_WIDTH = 128
MONO_HEIGHT = 64

logging.basicConfig(
    level=getattr(logging, LOG_LEVEL.upper()),
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

DEVICES_FILE = "devices.json"
app = Flask(__name__)

# Device registry:
# {
#   device_name: {
#       "screen_type": ...,
#       "last_seen": ...,
#       "message": ...,
#       "animation": ...
#   }
# }
devices = {}
devices_lock = Lock()


def load_devices():
    """Load device records from JSON."""
    global devices

    if not os.path.exists(DEVICES_FILE):
        devices = {}
        return

    try:
        with open(DEVICES_FILE, "r") as f:
            devices = json.load(f)

        logger.info("Loaded %d devices from %s", len(devices), DEVICES_FILE)

        # Ensure older records have an animation dictionary.
        for info in devices.values():
            info.setdefault("animation", {})
            info["animation"].setdefault("version", 0)
            info["animation"].setdefault("frames", [])
            info["animation"].setdefault("color_frames", [])
            info["animation"].setdefault("jpeg_frames", [])
            info["animation"].setdefault("rgb332_frames", [])

    except Exception as error:
        logger.error("Failed to load devices: %s", error)
        devices = {}


def save_devices():
    """Save device records. Animation frame bytes are Base64 strings."""
    try:
        with open(DEVICES_FILE, "w") as f:
            json.dump(devices, f, indent=2)
    except Exception as error:
        logger.error("Failed to save devices: %s", error)


load_devices()


def new_device(screen_type="ssd1306", last_seen=None):
    """Return a new device record."""
    return {
        "screen_type": screen_type,
        "last_seen": last_seen,
        "message": {"text": "Hello from your Mac!", "version": 1},
        "animation": {
            "version": 0,
            "frames": [],
            "color_frames": [],
            "jpeg_frames": [],
            "rgb332_frames": [],
        },
    }


def get_animation(device_id):
    """Return a device's animation dictionary, if present."""
    info = devices.get(device_id)
    if not info:
        return None

    if "animation" not in info:
        info["animation"] = {
            "version": 0,
            "frames": [],
            "color_frames": [],
            "jpeg_frames": [],
            "rgb332_frames": [],
        }

    return info["animation"]


def rgb_to_mono_frame(rgb):
    """
    Convert an RGB image to a 128x64, 1-bit-per-pixel frame for SSD1306
    devices: fit with aspect ratio preserved, centered on black, dithered.
    Rows are packed MSB-first (what Adafruit's drawBitmap expects),
    1024 bytes in total.
    """
    fitted = rgb.copy()
    fitted.thumbnail((MONO_WIDTH, MONO_HEIGHT), Image.Resampling.LANCZOS)

    canvas = Image.new("RGB", (MONO_WIDTH, MONO_HEIGHT), (0, 0, 0))
    canvas.paste(
        fitted,
        ((MONO_WIDTH - fitted.width) // 2, (MONO_HEIGHT - fitted.height) // 2),
    )

    # convert("1") applies Floyd-Steinberg dithering; white pixels are 1 bits.
    return canvas.convert("L").convert("1").tobytes()


@app.get("/")
def home():
    return render_template("index.html")


@app.get("/devices")
def devices_page():
    return render_template("devices.html")


@app.get("/api/message")
def get_message():
    device_id = request.args.get("device", "default")
    screen_type = request.args.get("screen", "ssd1306")

    logger.info(
        "Message requested by device %s (screen: %s)",
        device_id,
        screen_type,
    )

    with devices_lock:
        if device_id not in devices:
            devices[device_id] = new_device(screen_type=screen_type)
            logger.info("Auto-registered new device: %s", device_id)
        else:
            devices[device_id]["last_seen"] = datetime.now().isoformat()
            devices[device_id]["screen_type"] = screen_type

        save_devices()
        return jsonify(devices[device_id]["message"])


@app.post("/api/message")
def update_message():
    device_id = request.args.get("device", "default")
    data = request.get_json(silent=True) or {}
    text = data.get("text")

    if not isinstance(text, str):
        return jsonify(error="text must be a string"), 400

    text = text.strip()
    if not text:
        return jsonify(error="message cannot be empty"), 400
    if len(text) > 300:
        return jsonify(error="message must be 300 characters or fewer"), 400

    with devices_lock:
        if device_id not in devices:
            devices[device_id] = new_device()

        message_info = devices[device_id].setdefault(
            "message",
            {"text": "Hello from your Mac!", "version": 1},
        )
        message_version = int(message_info.get("version", 0)) + 1

        devices[device_id]["message"] = {
            "text": text,
            "version": message_version,
        }
        save_devices()
        return jsonify(devices[device_id]["message"])


@app.post("/api/animation")
def upload_animation():
    """Upload a GIF and store centered, pixel-budgeted RGB332 frames."""
    device_id = request.args.get("device", "default")

    uploaded = request.files.get("gif")
    if not uploaded or not uploaded.filename:
        return jsonify(error="Choose a GIF file"), 400

    raw = uploaded.read(MAX_GIF_BYTES + 1)
    if len(raw) > MAX_GIF_BYTES:
        return jsonify(error="GIF must be 5 MB or smaller"), 413

    try:
        image = Image.open(io.BytesIO(raw))
        if image.format != "GIF":
            return jsonify(error="The uploaded file must be a GIF"), 400

        rgba_frames = []
        content_bbox = None

        # Collect frames and find one bounding box around visible content
        # across the animation.
        for source_frame in ImageSequence.Iterator(image):
            if len(rgba_frames) >= MAX_FRAMES:
                break

            rgba = source_frame.convert("RGBA")
            rgba_frames.append(rgba.copy())

            bbox = rgba.getchannel("A").getbbox()
            if bbox:
                if content_bbox is None:
                    content_bbox = bbox
                else:
                    content_bbox = (
                        min(content_bbox[0], bbox[0]),
                        min(content_bbox[1], bbox[1]),
                        max(content_bbox[2], bbox[2]),
                        max(content_bbox[3], bbox[3]),
                    )

        if not rgba_frames:
            return jsonify(error="The GIF contains no frames"), 400

        # Fully transparent GIF: use a 1×1 black frame.
        if content_bbox is None:
            content_bbox = (0, 0, 1, 1)

        left, top, right, bottom = content_bbox
        crop_width = max(1, right - left)
        crop_height = max(1, bottom - top)
        crop_pixels = crop_width * crop_height

        # Fit within the display and the ESP32's pixel-count budget.
        # Unlike a fixed target rectangle, this lets wide or tall GIFs
        # use more of the available pixels.
        scale = min(
            SCREEN_WIDTH / crop_width,
            SCREEN_HEIGHT / crop_height,
            (ANIM_MAX_PIXELS / crop_pixels) ** 0.5,
        )

        # Floor dimensions so the resulting frame cannot exceed the budget.
        output_width = max(1, int(crop_width * scale))
        output_height = max(1, int(crop_height * scale))

        # Guard against a floating-point rounding edge case.
        while output_width * output_height > ANIM_MAX_PIXELS:
            if output_width / crop_width >= output_height / crop_height:
                output_width -= 1
            else:
                output_height -= 1

        rgb332_frames = []
        mono_frames = []

        for rgba in rgba_frames:
            cropped = rgba.crop(content_bbox)

            # Composite transparency onto black.
            black_background = Image.new(
                "RGBA",
                cropped.size,
                (0, 0, 0, 255),
            )
            black_background.alpha_composite(cropped)
            rgb = black_background.convert("RGB")

            if rgb.size != (output_width, output_height):
                rgb = rgb.resize(
                    (output_width, output_height),
                    Image.Resampling.LANCZOS,
                )

            # RGB332: RRR GGG BB, one byte per pixel.
            frame = bytearray()
            for r, g, b in rgb.getdata():
                frame.append(
                    (r & 0xE0) |
                    ((g & 0xE0) >> 3) |
                    (b >> 6)
                )

            rgb332_frames.append(bytes(frame))

            mono_frames.append(rgb_to_mono_frame(black_background.convert("RGB")))

    except Exception as error:
        logger.warning("GIF processing failed: %s", error)
        return jsonify(error="Could not read that GIF"), 400

    # JSON stores the frame bytes as Base64 strings.
    encoded_frames = [
        base64.b64encode(frame).decode("ascii")
        for frame in rgb332_frames
    ]

    encoded_mono_frames = [
        base64.b64encode(frame).decode("ascii")
        for frame in mono_frames
    ]

    x = (SCREEN_WIDTH - output_width) // 2
    y = (SCREEN_HEIGHT - output_height) // 2

    with devices_lock:
        if device_id not in devices:
            devices[device_id] = new_device(screen_type="st7789")

        anim = devices[device_id].setdefault("animation", {})
        version = int(anim.get("version", 0)) + 1

        devices[device_id]["animation"] = {
            "version": version,
            "frames": encoded_mono_frames,
            "color_frames": [],
            "jpeg_frames": [],
            "rgb332_frames": encoded_frames,
            "rgb332_width": output_width,
            "rgb332_height": output_height,
            "rgb332_x": x,
            "rgb332_y": y,
        }

        save_devices()

    logger.info(
        "Stored animation for %s: %d frames, %dx%d RGB332 (%d pixels)",
        device_id,
        len(encoded_frames),
        output_width,
        output_height,
        output_width * output_height,
    )

    return jsonify(
        version=version,
        frame_count=len(encoded_frames),
        width=output_width,
        height=output_height,
        x=x,
        y=y,
        format="rgb332",
        frame_bytes=output_width * output_height,
    )


@app.get("/api/animation")
def get_animation_info():
    """Legacy animation metadata endpoint."""
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        return jsonify(
            version=anim.get("version", 0),
            frame_count=len(anim.get("frames", [])),
            frame_ms=120,
        )


@app.get("/api/animation/frame/<int:index>")
def get_animation_frame(index):
    """Legacy monochrome animation frame endpoint."""
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        frames = anim.get("frames", [])
        if index < 0 or index >= len(frames):
            return jsonify(error="Frame not found"), 404

        frame = frames[index]

    if isinstance(frame, str):
        try:
            frame = base64.b64decode(frame)
        except Exception:
            return jsonify(error="Stored frame is invalid"), 500

    return Response(frame, mimetype="application/octet-stream")


@app.get("/api/animation/rgb565")
def get_color_animation_info():
    """Legacy RGB565 animation metadata endpoint."""
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        frames = anim.get("color_frames", [])
        return jsonify(
            version=anim.get("version", 0),
            frame_count=len(frames),
            frame_ms=120,
            width=128,
            height=64,
            format="rgb565-be",
            frame_bytes=128 * 64 * 2,
        )


@app.get("/api/animation/rgb565/frame/<int:index>")
def get_color_animation_frame(index):
    """Legacy RGB565 animation frame endpoint."""
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        frames = anim.get("color_frames", [])
        if index < 0 or index >= len(frames):
            return jsonify(error="Frame not found"), 404

        frame = frames[index]

    if isinstance(frame, str):
        try:
            frame = base64.b64decode(frame)
        except Exception:
            return jsonify(error="Stored frame is invalid"), 500

    return Response(frame, mimetype="application/octet-stream")


@app.get("/api/animation/jpeg")
def get_jpeg_animation_info():
    """
    Compatibility URL used by the ESP32 sketch.
    Despite the URL name, this reports raw RGB332 data, not JPEG.
    """
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        frames = anim.get("rgb332_frames", [])
        width = int(anim.get("rgb332_width", 0))
        height = int(anim.get("rgb332_height", 0))

        return jsonify(
            version=anim.get("version", 0),
            frame_count=len(frames),
            frame_ms=120,
            width=width,
            height=height,
            x=int(anim.get("rgb332_x", 0)),
            y=int(anim.get("rgb332_y", 0)),
            format="rgb332",
            frame_bytes=width * height,
        )


@app.get("/api/animation/jpeg/frame/<int:index>")
def get_jpeg_animation_frame(index):
    """
    Compatibility URL used by the ESP32 sketch.
    Returns raw RGB332 bytes, not JPEG data.
    """
    device_id = request.args.get("device", "default")

    with devices_lock:
        anim = get_animation(device_id)
        if anim is None:
            return jsonify(error="Device not found"), 404

        frames = anim.get("rgb332_frames", [])
        if index < 0 or index >= len(frames):
            return jsonify(error="Frame not found"), 404

        encoded_frame = frames[index]

    try:
        frame = base64.b64decode(encoded_frame)
    except Exception:
        logger.exception("Could not decode stored RGB332 frame")
        return jsonify(error="Stored frame is invalid"), 500

    return Response(frame, mimetype="application/octet-stream")


@app.get("/api/status")
def status():
    device_id = request.args.get("device", "default")

    with devices_lock:
        if device_id not in devices:
            return jsonify(error="Device not found"), 404

        last_seen_str = devices[device_id].get("last_seen")
        device_seen = False

        if last_seen_str:
            try:
                last_seen = datetime.fromisoformat(last_seen_str)
                device_seen = (
                    datetime.now() - last_seen
                    < timedelta(seconds=DEVICE_TIMEOUT_SECONDS)
                )
            except ValueError:
                logger.warning("Invalid last_seen value for %s", device_id)

        return jsonify(
            message=devices[device_id].get("message", {}),
            device_seen=device_seen,
        )


@app.get("/health")
def health():
    return jsonify(
        status="ok",
        timestamp=datetime.now().isoformat(),
    )


@app.get("/api/devices")
def list_devices():
    with devices_lock:
        device_list = []

        for name, info in devices.items():
            last_seen_str = info.get("last_seen")
            online = False

            if last_seen_str:
                try:
                    last_seen = datetime.fromisoformat(last_seen_str)
                    online = (
                        datetime.now() - last_seen
                        < timedelta(seconds=DEVICE_TIMEOUT_SECONDS)
                    )
                except ValueError:
                    logger.warning("Invalid last_seen value for %s", name)

            device_list.append({
                "name": name,
                "screen_type": info.get("screen_type", "unknown"),
                "last_seen": last_seen_str,
                "online": online,
            })

        return jsonify(devices=device_list)


@app.get("/api/devices/<name>")
def get_device(name):
    with devices_lock:
        if name not in devices:
            return jsonify(error="Device not found"), 404

        info = devices[name]
        anim = info.get("animation", {})
        last_seen_str = info.get("last_seen")
        online = False

        if last_seen_str:
            try:
                last_seen = datetime.fromisoformat(last_seen_str)
                online = (
                    datetime.now() - last_seen
                    < timedelta(seconds=DEVICE_TIMEOUT_SECONDS)
                )
            except ValueError:
                logger.warning("Invalid last_seen value for %s", name)

        return jsonify({
            "name": name,
            "screen_type": info.get("screen_type", "unknown"),
            "last_seen": last_seen_str,
            "online": online,
            "message": info.get("message", {}),
            "animation": {
                "version": anim.get("version", 0),
                "frame_count": len(anim.get("frames", [])),
                "color_frame_count": len(anim.get("color_frames", [])),
                "rgb332_frame_count": len(anim.get("rgb332_frames", [])),
                "rgb332_width": anim.get("rgb332_width", 0),
                "rgb332_height": anim.get("rgb332_height", 0),
            },
        })


@app.post("/api/devices")
def create_device():
    data = request.get_json(silent=True) or {}
    name = data.get("name")
    screen_type = data.get("screen_type", "ssd1306")

    if not name or not isinstance(name, str):
        return jsonify(error="name is required and must be a string"), 400

    name = name.strip()
    if not name:
        return jsonify(error="name cannot be empty"), 400

    with devices_lock:
        if name in devices:
            return jsonify(error="Device already exists"), 409

        devices[name] = new_device(screen_type=screen_type)
        save_devices()

    return jsonify(name=name, screen_type=screen_type, created=True)


@app.delete("/api/devices/<name>")
def delete_device(name):
    with devices_lock:
        if name not in devices:
            return jsonify(error="Device not found"), 404

        del devices[name]
        save_devices()

    return jsonify(name=name, deleted=True)


if __name__ == "__main__":
    logger.info("Starting server on %s:%d", HOST, PORT)
    app.run(host=HOST, port=PORT, debug=DEBUG)
