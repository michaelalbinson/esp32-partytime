"""Putting rendered programs onto devices, and taking them off."""

import logging
import time

from .config import ALL_DEVICES, EFFECTS, TEXT_STYLES
from .media.image import ImageError
from .program import WAIT, Effect, ProgramError, Text, build_animation
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
                "resume": _resume_for(old, show_id),
            }

        registry.save()


def _step_ref(step, message):
    if step["type"] == "gif":
        return step["gif"]
    if step["type"] == "effect":
        return Effect(step["effect"])
    if step["type"] == "text":
        # Text left blank shows the display's own message as it is now. (A new
        # message ends the show on that display anyway.)
        return Text(step["style"], step["text"] or message)
    return WAIT


def _resume_for(old, new_show_id):
    """
    What a device should go back to when the show it is about to play stops:
    the GIF it was looping, if any. A show started over another show inherits
    that show's answer, so stopping always returns to the state from before
    any show.
    """
    if new_show_id is None:
        return None  # a plain upload has nothing to go back to

    if old.get("show_id"):
        return old.get("resume")

    clips = old.get("clips") or []
    if old.get("steps") and clips and clips[0].get("gif"):
        return {"gif": clips[0]["gif"]}

    return None


def lane_steps(lane, message=""):
    """
    A lane's steps as (reference, duration in ms), where the reference is a
    GIF id, Effect, Text or WAIT. message fills in text steps left blank.
    """
    return [
        (_step_ref(step, message), round(step["seconds"] * 1000))
        for step in lane["steps"]
    ]


def expand_lanes(show, device_names):
    """
    The (device, lane) pairs a show plays. A lane for all displays covers every
    display that has no lane of its own in the show, as they are registered now.
    """
    own = {lane["device"] for lane in show["lanes"] if lane["device"] != ALL_DEVICES}

    pairs = []
    for lane in show["lanes"]:
        if lane["device"] == ALL_DEVICES:
            pairs.extend((name, lane) for name in device_names if name not in own)
        else:
            pairs.append((lane["device"], lane))

    return pairs


def play_show(registry, library, show):
    """
    Render every lane of a show and start them together. Nothing changes on
    any device unless every lane renders. Raises PlaybackError.
    """
    with registry.lock:
        device_names = list(registry.devices)

    missing = [
        lane["device"] for lane in show["lanes"]
        if lane["device"] != ALL_DEVICES and lane["device"] not in device_names
    ]
    if missing:
        raise PlaybackError("Unknown display: " + ", ".join(missing), status=404)

    pairs = expand_lanes(show, device_names)
    if not pairs:
        raise PlaybackError("There are no displays to play this show on yet")

    sources = {}
    try:
        for lane in show["lanes"]:
            for step in lane["steps"]:
                gif_id = step.get("gif")
                if gif_id and gif_id not in sources:
                    sources[gif_id] = library.load_source(gif_id)
    except ImageError as error:
        raise PlaybackError(str(error)) from error

    with registry.lock:
        messages = {
            device: registry.devices.get(device, {}).get("message", {}).get("text", "")
            for device, _ in pairs
        }

    # Displays given the same steps share one render (frames are stored for
    # every display type, so only text left blank can differ between them).
    renders = {}
    animations = {}
    notes = {}
    try:
        for device, lane in pairs:
            steps = lane_steps(lane, messages[device])
            key = tuple(steps)
            if key not in renders:
                renders[key] = build_animation(steps, sources)
            animations[device], notes[device] = renders[key]
    except ProgramError as error:
        label = "All displays" if lane["device"] == ALL_DEVICES else lane["device"]
        raise PlaybackError(f"{label}: {error}") from error

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
EFFECT_LABELS = {
    "rain": "Digital rain",
    "static": "Static",
    "equalizer": "Equalizer",
    "scanner": "Scanner",
    "radar": "Radar",
    "testpattern": "Test pattern",
    "snow": "Snowfall",
    "confetti": "Confetti",
    "bounce": "Bouncing name",
    "dvd": "DVD logo",
}
STYLE_NAMES = {code: name for name, code in TEXT_STYLES.items()}
STYLE_LABELS = {"marquee": "Marquee", "flash": "Flash words", "wave": "Wavy text"}


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
    elif step and step["clip"] == -3:
        style = STYLE_NAMES.get(step.get("style"), "text")
        texts = anim.get("texts", [])
        text = texts[step["text"]] if 0 <= step.get("text", -1) < len(texts) else ""
        text = text.replace("\n", " / ")
        result.update(kind="text", name=f'{STYLE_LABELS.get(style, style)}: "{text}"')
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


def _render_resume(library, resume):
    """Re-render the GIF a device was looping before a show, or None."""
    if not resume or not resume.get("gif"):
        return None

    try:
        source = library.load_source(resume["gif"])
        animation, _ = build_animation(
            [(resume["gif"], source.duration_ms)], {resume["gif"]: source}
        )
    except (ImageError, ProgramError):
        return None  # it was deleted or no longer renders: show the message

    return animation


def stop_show(registry, library, show_id):
    """
    Stop a show's lanes. Each device goes back to the GIF it was looping before
    the show, or to its message if it wasn't playing anything. Returns
    (stopped, restored): the device names stopped, and those put back on a GIF.
    """
    with registry.lock:
        stopped = playing_devices(registry, show_id)
        resumes = {
            name: registry.devices[name]["animation"].get("resume")
            for name in stopped
        }

    # Rendering takes a moment; don't hold the lock for it.
    renders = {name: _render_resume(library, resume) for name, resume in resumes.items()}

    restored = []
    with registry.lock:
        for name in stopped:
            info = registry.devices.get(name)
            # Skip a device that has moved on (a new message, another show).
            if not info or info["animation"].get("show_id") != show_id:
                continue

            if renders[name]:
                install(registry, {name: renders[name]})
                restored.append(name)
            else:
                registry.clear_animation(name)

        if stopped:
            registry.save()

    return stopped, restored
