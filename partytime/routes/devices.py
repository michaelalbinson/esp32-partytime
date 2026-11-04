"""Device management API."""

from flask import Blueprint, jsonify, request

from ..playback import now_playing
from ..registry import new_device
from ..services import services

bp = Blueprint("devices", __name__)


@bp.get("/api/devices")
def list_devices():
    services_ = services()
    registry = services_.registry

    with registry.lock:
        return jsonify(devices=[
            {
                "name": name,
                "screen_type": info.get("screen_type", "unknown"),
                "last_seen": info.get("last_seen"),
                "online": registry.is_online(info, name),
                "now_playing": now_playing(
                    info.get("animation", {}),
                    services_.library,
                    services_.shows.shows,
                ),
            }
            for name, info in registry.devices.items()
        ])


@bp.get("/api/devices/<name>")
def get_device(name):
    services_ = services()
    registry = services_.registry

    with registry.lock:
        if name not in registry.devices:
            return jsonify(error="Device not found"), 404

        info = registry.devices[name]
        anim = info.get("animation", {})
        first_clip = (anim.get("clips") or [{}])[0]

        return jsonify({
            "name": name,
            "screen_type": info.get("screen_type", "unknown"),
            "last_seen": info.get("last_seen"),
            "online": registry.is_online(info, name),
            "message": info.get("message", {}),
            "now_playing": now_playing(
                anim, services_.library, services_.shows.shows
            ),
            "animation": {
                "version": anim.get("version", 0),
                "frame_count": len(anim.get("frames", [])),
                "rgb332_frame_count": len(anim.get("rgb332_frames", [])),
                "rgb332_width": first_clip.get("width", 0),
                "rgb332_height": first_clip.get("height", 0),
                "show_id": anim.get("show_id"),
            },
        })


@bp.post("/api/devices")
def create_device():
    registry = services().registry
    data = request.get_json(silent=True) or {}
    name = data.get("name")
    screen_type = data.get("screen_type", "ssd1306")

    if not name or not isinstance(name, str):
        return jsonify(error="name is required and must be a string"), 400

    name = name.strip()
    if not name:
        return jsonify(error="name cannot be empty"), 400

    with registry.lock:
        if name in registry.devices:
            return jsonify(error="Device already exists"), 409

        registry.devices[name] = new_device(screen_type=screen_type)
        registry.save()

    return jsonify(name=name, screen_type=screen_type, created=True)


@bp.delete("/api/devices/<name>")
def delete_device(name):
    registry = services().registry

    with registry.lock:
        if name not in registry.devices:
            return jsonify(error="Device not found"), 404

        del registry.devices[name]
        registry.save()

    return jsonify(name=name, deleted=True)
