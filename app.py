from flask import Flask, jsonify, request, render_template, Response
from threading import Lock
import io
import os
import logging
from datetime import datetime, timedelta
from PIL import Image, ImageSequence
from dotenv import load_dotenv

# Load environment variables from .env file if present
load_dotenv()

# Configuration
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "5050"))
DEBUG = os.getenv("DEBUG", "False").lower() in ("true", "1", "t")
MAX_GIF_BYTES = int(os.getenv("MAX_GIF_BYTES", str(5 * 1024 * 1024)))
MAX_FRAMES = int(os.getenv("MAX_FRAMES", "60"))
DEVICE_TIMEOUT_SECONDS = int(os.getenv("DEVICE_TIMEOUT_SECONDS", "10"))
LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO")

# Configure logging
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL.upper()),
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

app = Flask(__name__)

message = {"text": "Hello from your Mac!", "version": 1}
message_lock = Lock()

animation_lock = Lock()
animation = {
    "version": 0,
    "frames": [],
    "color_frames": [],
}

# Device tracking
device_last_seen = None
device_lock = Lock()

@app.get("/")
def home():
    return render_template("index.html")

@app.get("/api/message")
def get_message():
    with message_lock:
        current = dict(message)
    device_id = request.args.get("device_id", "unknown")
    logger.info("Message requested by device %s", device_id)

    # Update device last seen time
    with device_lock:
        device_last_seen = datetime.now()

    return jsonify(current)

@app.post("/api/message")
def update_message():
    global message
    data = request.get_json(silent=True) or {}
    text = data.get("text")

    if not isinstance(text, str):
        return jsonify(error="text must be a string"), 400

    text = text.strip()
    if not text:
        return jsonify(error="message cannot be empty"), 400
    if len(text) > 300:
        return jsonify(error="message must be 300 characters or fewer"), 400

    with message_lock:
        message = {"text": text, "version": message["version"] + 1}
        current = dict(message)

    return jsonify(current)

@app.post("/api/animation")
def upload_animation():
    global animation

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

        frames = []
        color_frames = []

        for source_frame in ImageSequence.Iterator(image):
            if len(frames) >= MAX_FRAMES:
                break

            rgba = source_frame.convert("RGBA")
            background = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
            background.alpha_composite(rgba)

            # Prepare a shared 128x64 image, preserving aspect ratio.
            rgb_frame = background.convert("RGB")
            rgb_frame.thumbnail((128, 64), Image.Resampling.LANCZOS)

            rgb_canvas = Image.new("RGB", (128, 64), (0, 0, 0))
            x = (128 - rgb_frame.width) // 2
            y = (64 - rgb_frame.height) // 2
            rgb_canvas.paste(rgb_frame, (x, y))

            # Original monochrome format: 128x64, 1 bit per pixel.
            gray_canvas = rgb_canvas.convert("L")
            pixels = gray_canvas.load()
            mono = bytearray()

            for py in range(64):
                for byte_x in range(16):
                    value = 0
                    for bit in range(8):
                        px = byte_x * 8 + bit
                        if pixels[px, py] >= 128:
                            value |= 1 << (7 - bit)
                    mono.append(value)

            frames.append(bytes(mono))

            # New color format: 128x64 RGB565, high byte first.
            color = bytearray()
            for r, g, b in rgb_canvas.getdata():
                rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                color.append((rgb565 >> 8) & 0xFF)
                color.append(rgb565 & 0xFF)

            color_frames.append(bytes(color))

        if not frames:
            return jsonify(error="The GIF contains no frames"), 400

    except Exception as error:
        logger.warning("GIF processing failed: %s", error)
        return jsonify(error="Could not read that GIF"), 400

    with animation_lock:
        animation = {
            "version": animation["version"] + 1,
            "frames": frames,
            "color_frames": color_frames,
        }
        version = animation["version"]

    return jsonify(version=version, frame_count=len(frames))


@app.get("/api/animation")
def get_animation_info():
    with animation_lock:
        return jsonify(
            version=animation["version"],
            frame_count=len(animation["frames"]),
            frame_ms=120,
        )


@app.get("/api/animation/frame/<int:index>")
def get_animation_frame(index):
    with animation_lock:
        if index < 0 or index >= len(animation["frames"]):
            return jsonify(error="Frame not found"), 404
        frame = animation["frames"][index]

    return Response(frame, mimetype="application/octet-stream")

@app.get("/api/status")
def status():
    # The ESP32 requests /api/message every two seconds.
    with message_lock:
        current = dict(message)

    # Check if device was seen recently
    with device_lock:
        device_seen = (
            device_last_seen is not None and
            datetime.now() - device_last_seen < timedelta(seconds=DEVICE_TIMEOUT_SECONDS)
        )

    return jsonify(message=current, device_seen=device_seen)

@app.get("/api/animation/rgb565")
def get_color_animation_info():
    with animation_lock:
        return jsonify(
            version=animation["version"],
            frame_count=len(animation["color_frames"]),
            frame_ms=120,
            width=128,
            height=64,
            format="rgb565-be",
            frame_bytes=128 * 64 * 2,
        )

@app.get("/api/animation/rgb565/frame/<int:index>")
def get_color_animation_frame(index):
    with animation_lock:
        if index < 0 or index >= len(animation["color_frames"]):
            return jsonify(error="Frame not found"), 404
        frame = animation["color_frames"][index]

    return Response(frame, mimetype="application/octet-stream")


@app.get("/health")
def health():
    """Health check endpoint for monitoring"""
    return jsonify(status="ok", timestamp=datetime.now().isoformat())


if __name__ == "__main__":
    # Local-network access is needed so the ESP32 can reach this server.
    logger.info(f"Starting server on {HOST}:{PORT}")
    app.run(host=HOST, port=PORT, debug=DEBUG)