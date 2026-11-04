"""What devices fetch to play animations, and the one-step GIF upload."""

import base64
import logging

from flask import Blueprint, Response, jsonify, request

from .. import config
from ..media.image import ImageError, read_image
from ..playback import install
from ..program import build_animation
from ..protocol import color_metadata, mono_metadata
from ..services import services

logger = logging.getLogger(__name__)

bp = Blueprint("animation", __name__)

# Devices created by an upload before they have ever polled.
UPLOAD_SCREEN_TYPE = "st7789"


def _animation_or_404(device_id):
    """Return (animation, None), or (None, error response) if no such device."""
    info = services().registry.devices.get(device_id)
    if not info:
        return None, (jsonify(error="Device not found"), 404)

    return info["animation"], None


def _frame_response(device_id, index, key):
    """Serve one stored frame from the animation's list under key."""
    registry = services().registry

    with registry.lock:
        anim, error = _animation_or_404(device_id)
        if error:
            return error

        frames = anim.get(key, [])
        if index < 0 or index >= len(frames):
            return jsonify(error="Frame not found"), 404

        encoded_frame = frames[index]

    try:
        frame = base64.b64decode(encoded_frame)
    except Exception:
        logger.exception("Could not decode stored frame")
        return jsonify(error="Stored frame is invalid"), 500

    return Response(frame, mimetype="application/octet-stream")


@bp.post("/api/animation")
def upload_animation():
    """
    Upload a GIF (or PNG) and play it on a device straight away. It also joins
    the library, so it can be used in shows.
    """
    services_ = services()
    device_id = request.args.get("device", "default")

    uploaded = request.files.get("gif")
    if not uploaded or not uploaded.filename:
        return jsonify(error="Choose a GIF file"), 400

    raw = uploaded.read(config.MAX_GIF_BYTES + 1)
    if len(raw) > config.MAX_GIF_BYTES:
        return jsonify(error="GIF must be 5 MB or smaller"), 413

    try:
        source = read_image(raw)
        entry = services_.library.add(raw, uploaded.filename, source)
        animation, _ = build_animation(
            [(entry["id"], source.duration_ms)],
            {entry["id"]: source},
        )
    except ImageError as error:
        logger.warning("GIF processing failed: %s", error)
        return jsonify(error=str(error)), 400

    registry = services_.registry
    with registry.lock:
        registry.ensure(device_id, screen_type=UPLOAD_SCREEN_TYPE)
        install(registry, {device_id: animation})
        stored = registry.devices[device_id]["animation"]
        metadata = color_metadata(stored)

    logger.info(
        "Stored animation for %s: %d frames, %dx%d RGB332 (%d pixels)",
        device_id,
        metadata["frame_count"],
        metadata["width"],
        metadata["height"],
        metadata["frame_bytes"],
    )

    return jsonify(**metadata, gif_id=entry["id"])


@bp.get("/api/animation")
def get_animation_info():
    """Metadata for the 1-bit frames."""
    registry = services().registry
    device_id = request.args.get("device", "default")

    with registry.lock:
        anim, error = _animation_or_404(device_id)
        if error:
            return error

        return jsonify(mono_metadata(anim))


@bp.get("/api/animation/frame/<int:index>")
def get_animation_frame(index):
    """One 1-bit frame: 1,024 bytes, 128x64, packed MSB-first."""
    device_id = request.args.get("device", "default")
    return _frame_response(device_id, index, "frames")


@bp.get("/api/animation/jpeg")
def get_color_animation_info():
    """
    Metadata for the color frames. The URL is a compatibility name used by the
    ESP32 sketch: despite it, this reports raw RGB332 data, not JPEG.
    """
    registry = services().registry
    device_id = request.args.get("device", "default")

    with registry.lock:
        anim, error = _animation_or_404(device_id)
        if error:
            return error

        return jsonify(color_metadata(anim))


@bp.get("/api/animation/jpeg/frame/<int:index>")
def get_color_animation_frame(index):
    """One raw RGB332 frame (see get_color_animation_info)."""
    device_id = request.args.get("device", "default")
    return _frame_response(device_id, index, "rgb332_frames")
