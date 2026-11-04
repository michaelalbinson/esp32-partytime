"""Text messages, and the status the web UI polls."""

import logging
from datetime import datetime

from flask import Blueprint, jsonify, request

from ..registry import DEFAULT_MESSAGE, new_device
from ..services import services

logger = logging.getLogger(__name__)

bp = Blueprint("messages", __name__)

MAX_MESSAGE_LENGTH = 300


@bp.get("/api/message")
def get_message():
    registry = services().registry
    device_id = request.args.get("device", "default")
    screen_type = request.args.get("screen", "ssd1306")

    logger.info(
        "Message requested by device %s (screen: %s)",
        device_id,
        screen_type,
    )

    with registry.lock:
        if device_id not in registry.devices:
            registry.devices[device_id] = new_device(screen_type=screen_type)
            logger.info("Auto-registered new device: %s", device_id)
        else:
            registry.devices[device_id]["last_seen"] = datetime.now().isoformat()
            registry.devices[device_id]["screen_type"] = screen_type

        registry.save()
        return jsonify(registry.devices[device_id]["message"])


@bp.post("/api/message")
def update_message():
    registry = services().registry
    device_id = request.args.get("device", "default")
    data = request.get_json(silent=True) or {}
    text = data.get("text")

    if not isinstance(text, str):
        return jsonify(error="text must be a string"), 400

    text = text.strip()
    if not text:
        return jsonify(error="message cannot be empty"), 400
    if len(text) > MAX_MESSAGE_LENGTH:
        return jsonify(
            error=f"message must be {MAX_MESSAGE_LENGTH} characters or fewer"
        ), 400

    with registry.lock:
        device = registry.ensure(device_id)

        message_info = device.setdefault(
            "message",
            {"text": DEFAULT_MESSAGE, "version": 1},
        )
        message_version = int(message_info.get("version", 0)) + 1

        device["message"] = {
            "text": text,
            "version": message_version,
        }

        # A new message replaces any playing animation. Bumping the version
        # tells devices the animation changed, so they stop and show the text.
        anim = device.get("animation", {})
        if anim.get("steps"):
            registry.clear_animation(device_id)

        registry.save()
        return jsonify(device["message"])


@bp.get("/api/status")
def status():
    registry = services().registry
    device_id = request.args.get("device", "default")

    with registry.lock:
        if device_id not in registry.devices:
            return jsonify(error="Device not found"), 404

        info = registry.devices[device_id]

        return jsonify(
            message=info.get("message", {}),
            device_seen=registry.is_online(info, device_id),
        )
