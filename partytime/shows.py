"""
Shows: saved sequences of GIFs across displays.

A show has one lane per display. A lane is an ordered list of steps, each a GIF
shown for a number of seconds, a wait, an effect (digital rain, static, equalizer...), or text (marquee, flash words, wavy). Lanes start together and loop, so the
same GIFs can play in sync on several displays, or staggered with waits so they
chase from one display to the next.
"""

import time
import uuid
from threading import RLock

from . import config
from .storage import load_json, save_json


class ValidationError(Exception):
    """The show is malformed. The message is safe to show the user."""


def _clean_seconds(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError("Each step needs a number of seconds")

    if not config.MIN_STEP_SECONDS <= value <= config.MAX_STEP_SECONDS:
        raise ValidationError(
            f"Step length must be between {config.MIN_STEP_SECONDS} and "
            f"{config.MAX_STEP_SECONDS} seconds"
        )

    return round(float(value), 3)


def _clean_step(step, library):
    if not isinstance(step, dict):
        raise ValidationError("Each step must be an object")

    seconds = _clean_seconds(step.get("seconds"))
    kind = step.get("type")

    if kind == "wait":
        return {"type": "wait", "seconds": seconds}

    if kind == "effect":
        effect = step.get("effect")
        if effect not in config.EFFECTS:
            raise ValidationError("Unknown effect")
        return {"type": "effect", "effect": effect, "seconds": seconds}

    if kind == "text":
        style = step.get("style")
        if style not in config.TEXT_STYLES:
            raise ValidationError("Unknown text style")

        text = step.get("text", "")
        if not isinstance(text, str):
            raise ValidationError("Step text must be text")
        text = text.strip()
        if len(text) > config.MAX_TEXT_LENGTH:
            raise ValidationError(
                f"Step text must be {config.MAX_TEXT_LENGTH} characters or fewer"
            )

        # Blank text means "show the display's own message".
        return {"type": "text", "style": style, "text": text, "seconds": seconds}

    if kind == "gif":
        gif_id = step.get("gif")
        if not isinstance(gif_id, str) or not library.get(gif_id):
            raise ValidationError("A step uses a GIF that isn't in the library")
        return {"type": "gif", "gif": gif_id, "seconds": seconds}

    raise ValidationError('A step\'s type must be "gif", "wait", "effect" or "text"')


def clean_show(payload, library):
    """Validate and normalize a show's editable fields (name and lanes)."""
    if not isinstance(payload, dict):
        raise ValidationError("Request body must be a JSON object")

    name = payload.get("name")
    if not isinstance(name, str) or not name.strip():
        raise ValidationError("A show needs a name")
    name = name.strip()
    if len(name) > config.MAX_NAME_LENGTH:
        raise ValidationError(
            f"Show names must be {config.MAX_NAME_LENGTH} characters or fewer"
        )

    lanes = payload.get("lanes")
    if not isinstance(lanes, list) or not lanes:
        raise ValidationError("A show needs at least one lane")
    if len(lanes) > config.MAX_LANES:
        raise ValidationError(f"A show can have at most {config.MAX_LANES} lanes")

    cleaned = []
    seen_devices = set()

    for lane in lanes:
        if not isinstance(lane, dict):
            raise ValidationError("Each lane must be an object")

        device = lane.get("device")
        if not isinstance(device, str) or not device.strip():
            raise ValidationError("Each lane needs a display")
        device = device.strip()

        if device in seen_devices:
            raise ValidationError(f'"{device}" is in more than one lane')
        seen_devices.add(device)

        steps = lane.get("steps")
        if not isinstance(steps, list) or not steps:
            raise ValidationError(f'Lane "{device}" needs at least one step')
        if len(steps) > config.MAX_LANE_STEPS:
            raise ValidationError(
                f'Lane "{device}" can have at most {config.MAX_LANE_STEPS} steps'
            )

        cleaned.append({
            "device": device,
            "steps": [_clean_step(step, library) for step in steps],
        })

    return {"name": name, "lanes": cleaned}


class ShowStore:
    """Saved shows, persisted to a JSON file."""

    def __init__(self, path, library):
        self.path = path
        self.library = library
        self.lock = RLock()
        self.shows = load_json(path, {})

    def list(self):
        with self.lock:
            return sorted(self.shows.values(), key=lambda s: s["name"].lower())

    def get(self, show_id):
        with self.lock:
            return self.shows.get(show_id)

    def create(self, payload):
        fields = clean_show(payload, self.library)
        now = time.time()

        with self.lock:
            show = {
                "id": uuid.uuid4().hex[:8],
                **fields,
                "created_at": now,
                "updated_at": now,
            }
            self.shows[show["id"]] = show
            save_json(self.path, self.shows)
            return show

    def update(self, show_id, payload):
        """Replace a show's name and lanes. Returns None if it doesn't exist."""
        fields = clean_show(payload, self.library)

        with self.lock:
            show = self.shows.get(show_id)
            if show is None:
                return None

            show.update(fields, updated_at=time.time())
            save_json(self.path, self.shows)
            return show

    def delete(self, show_id):
        with self.lock:
            if show_id not in self.shows:
                return False

            del self.shows[show_id]
            save_json(self.path, self.shows)
            return True

    def names_using_gif(self, gif_id):
        """Names of the shows that have a step using this GIF."""
        with self.lock:
            return [
                show["name"]
                for show in self.shows.values()
                if any(
                    step.get("gif") == gif_id
                    for lane in show["lanes"]
                    for step in lane["steps"]
                )
            ]
