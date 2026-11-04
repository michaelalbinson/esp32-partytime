"""Device registry: who the devices are, what they show, and persistence."""

import logging
import time
from datetime import datetime, timedelta
from threading import RLock

from .storage import load_json, save_json

logger = logging.getLogger(__name__)

DEFAULT_MESSAGE = "Hello from your Mac!"


def empty_animation(version=0):
    """
    A device's animation record with nothing playing.

    A playing record also carries "frames" (1-bit frames, base64),
    "rgb332_frames", "frame_delays_ms" (one per frame, all clips back to back),
    "clips", "steps", "cycle_ms", "started_at" and "show_id" -- see program.py.
    "resume" is set while a show plays: the library GIF ({"gif": id}) the device
    was looping before the show, to go back to when the show stops.
    """
    return {
        "version": version,
        "frames": [],
        "rgb332_frames": [],
        "frame_delays_ms": [],
        "clips": [],
        "steps": [],
        "texts": [],
        "cycle_ms": 0,
        "started_at": 0,
        "show_id": None,
        "resume": None,
    }


def new_device(screen_type="ssd1306", last_seen=None):
    """Return a new device record."""
    return {
        "screen_type": screen_type,
        "last_seen": last_seen,
        "message": {"text": DEFAULT_MESSAGE, "version": 1},
        "animation": empty_animation(),
    }


def normalize_animation(anim):
    """
    Fill in fields missing from older records. An animation stored before
    shows existed is one GIF, which becomes a single step that loops.
    """
    for key, value in empty_animation(anim.get("version", 0)).items():
        anim.setdefault(key, value)

    # Legacy placeholders no longer used.
    anim.pop("color_frames", None)
    anim.pop("jpeg_frames", None)

    if not anim["clips"] and (anim["frames"] or anim["rgb332_frames"]):
        count = len(anim["rgb332_frames"] or anim["frames"])
        delays = anim["frame_delays_ms"]
        if len(delays) != count:
            delays = anim["frame_delays_ms"] = [120] * count
        total = sum(delays)

        anim["clips"] = [{
            "first_frame": 0,
            "frame_count": count,
            "width": int(anim.pop("rgb332_width", 0)),
            "height": int(anim.pop("rgb332_height", 0)),
            "x": int(anim.pop("rgb332_x", 0)),
            "y": int(anim.pop("rgb332_y", 0)),
        }]
        anim["steps"] = [{"clip": 0, "start_ms": 0, "duration_ms": total}]
        anim["cycle_ms"] = total
        anim["started_at"] = time.time()

    for legacy_key in ("rgb332_width", "rgb332_height", "rgb332_x", "rgb332_y"):
        anim.pop(legacy_key, None)

    return anim


class DeviceRegistry:
    """
    Device records keyed by name, persisted to a JSON file.

    Hold `lock` while reading or changing `devices`, and call `save()` after a
    change. The lock is re-entrant so helpers can nest.
    """

    def __init__(self, path, timeout_seconds):
        self.path = path
        self.timeout_seconds = timeout_seconds
        self.lock = RLock()
        self.devices = {}
        self.load()

    def load(self):
        try:
            self.devices = load_json(self.path, {})
            logger.info("Loaded %d devices from %s", len(self.devices), self.path)

            for info in self.devices.values():
                info["animation"] = normalize_animation(info.get("animation", {}))
        except Exception as error:
            logger.error("Failed to load devices: %s", error)
            self.devices = {}

    def save(self):
        """Persist device records. Animation frame bytes are Base64 strings."""
        try:
            save_json(self.path, self.devices)
        except Exception as error:
            logger.error("Failed to save devices: %s", error)

    def ensure(self, name, screen_type="ssd1306"):
        """Return the named device record, creating it if it is new."""
        if name not in self.devices:
            self.devices[name] = new_device(screen_type=screen_type)
        return self.devices[name]

    def is_online(self, info, name=""):
        """Whether the device has polled within the timeout window."""
        last_seen_str = info.get("last_seen")
        if not last_seen_str:
            return False

        try:
            last_seen = datetime.fromisoformat(last_seen_str)
        except ValueError:
            logger.warning("Invalid last_seen value for %s", name)
            return False

        return datetime.now() - last_seen < timedelta(seconds=self.timeout_seconds)

    def clear_animation(self, name):
        """Stop whatever a device is playing. Caller holds the lock and saves."""
        anim = self.devices[name].get("animation", {})
        self.devices[name]["animation"] = empty_animation(
            int(anim.get("version", 0)) + 1
        )
