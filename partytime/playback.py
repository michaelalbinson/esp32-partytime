"""Putting rendered programs onto devices, and taking them off."""

import logging
import time

from .config import EFFECTS
from .media.gif import GifError
from .program import WAIT, Effect, ProgramError, build_animation
from .protocol import cycle_position_ms

logger = logging.getLogger(__name__)


class PlaybackError(Exception):
    """A show can't be played. The message is safe to show the user."""

    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def install(registry, animations, show_id=None, started_at=None, cycle_ms=None):
    """
    Start rendered animations on devices (device name -> animation from
    build_animation). Every device gets the same start time, and so stays in
    step with the others. The devices must exist.
    """
    started_at = time.time() if started_at is None else started_at

    with registry.lock:
        missing = [name for name in animations if name not in registry.devices]
        if missing:
            raise PlaybackError(
                "Unknown display: " + ", ".join(missing), status=404
            )

        for name, anim in animations.items():
            old = registry.devices[name].get("animation", {})
            registry.devices[name]["animation"] = {
                **anim,
                "version": int(old.get("version", 0)) + 1,
                "started_at": started_at,
                "cycle_ms": cycle_ms or anim["cycle_ms"],
                "show_id": show_id,
            }

        registry.save()


def _step_ref(step):
    if step["type"] == "gif":
        return step["gif"]
    if step["type"] == "effect":
        return Effect(step["effect"])
    return WAIT


def lane_steps(lane):
    """A lane's steps as (gif id, Effect or WAIT, duration in ms)."""
    return [
        (_step_ref(step), round(step["seconds"] * 1000))
        for step in lane["steps"]
    ]


def play_show(registry, library, show):
    """
    Render every lane of a show and start them together. Nothing changes on
    any device unless every lane renders. Raises PlaybackError.
    """
    with registry.lock:
        missing = [
            lane["device"] for lane in show["lanes"]
            if lane["device"] not in registry.devices
        ]
    if missing:
        raise PlaybackError("Unknown display: " + ", ".join(missing), status=404)

    sources = {}
    try:
        for lane in show["lanes"]:
            for step in lane["steps"]:
                gif_id = step.get("gif")
                if gif_id and gif_id not in sources:
                    sources[gif_id] = library.load_source(gif_id)
    except GifError as error:
        raise PlaybackError(str(error)) from error

    animations = {}
    notes = {}
    try:
        for lane in show["lanes"]:
            animations[lane["device"]], notes[lane["device"]] = build_animation(
                lane_steps(lane), sources
            )
    except ProgramError as error:
        raise PlaybackError(f'{lane["device"]}: {error}') from error

    # Every lane runs for the length of the longest, so lanes stay in step
    # however their steps add up; a shorter lane idles at the end.
    cycle_ms = max(anim["cycle_ms"] for anim in animations.values())
    started_at = time.time()

    install(
        registry,
        animations,
        show_id=show["id"],
        started_at=started_at,
        cycle_ms=cycle_ms,
    )

    logger.info(
        "Started show %s (%s) on %s, cycle %d ms",
        show["id"], show["name"], ", ".join(animations), cycle_ms,
    )

    return {
        "started_at": started_at,
        "cycle_ms": cycle_ms,
        "lanes": [
            {
                "device": name,
                "frame_count": len(anim["rgb332_frames"]),
                "notes": notes[name],
            }
            for name, anim in animations.items()
        ],
    }


EFFECT_NAMES = {code: name for name, code in EFFECTS.items()}
EFFECT_LABELS = {"rain": "Digital rain", "static": "Static"}


def now_playing(anim, library, shows, now=None):
    """
    What a device is showing right now, from its program and the show clock:
    {"kind": "gif" | "effect" | "wait", "name": ..., "show": show name or None}.
    None if no program is assigned. Between steps a lane idles, which reads as
    a wait.
    """
    if not anim.get("steps"):
        return None

    position = cycle_position_ms(anim, now)
    step = next(
        (s for s in anim["steps"]
         if s["start_ms"] <= position < s["start_ms"] + s["duration_ms"]),
        None,
    )

    show = shows.get(anim.get("show_id")) if anim.get("show_id") else None
    result = {"kind": "wait", "name": "Waiting (showing text)",
              "show": show["name"] if show else None}

    if step and step["clip"] >= 0:
        gif_id = anim["clips"][step["clip"]].get("gif")
        entry = library.get(gif_id) if gif_id else None
        result.update(kind="gif", name=entry["name"] if entry else "GIF")
    elif step and step["clip"] == -2:
        effect = EFFECT_NAMES.get(step.get("effect"), "effect")
        result.update(kind="effect", name=EFFECT_LABELS.get(effect, effect))

    return result


def playing_devices(registry, show_id):
    """Names of the devices currently playing a lane of this show."""
    with registry.lock:
        return [
            name for name, info in registry.devices.items()
            if info.get("animation", {}).get("show_id") == show_id
            and info["animation"].get("steps")
        ]


def stop_show(registry, show_id):
    """Stop a show's lanes. Returns the names of the devices stopped."""
    with registry.lock:
        stopped = playing_devices(registry, show_id)
        for name in stopped:
            registry.clear_animation(name)

        if stopped:
            registry.save()

    return stopped
