"""
What devices are told about their animation.

The metadata keeps the single-animation fields the firmware has always read
(frame_count, frame_ms, frame_delays, and for color displays the frame's
geometry). Firmware that understands programs also reads "program": the steps
to play, and where in the cycle the show is right now, so every display of a
show stays in step.
"""

import time

from . import config


def frame_timing(anim, frame_count):
    """
    Build the timing fields devices need: per-frame delays, plus the mean as
    frame_ms for firmware that only understands a single delay.
    """
    delays = anim.get("frame_delays_ms") or []

    if len(delays) != frame_count:
        delays = [config.DEFAULT_FRAME_MS] * frame_count

    frame_ms = (
        round(sum(delays) / len(delays)) if delays else config.DEFAULT_FRAME_MS
    )

    return {"frame_ms": frame_ms, "frame_delays": delays}


def cycle_position_ms(anim, now=None):
    """Where in its cycle the animation is at time now (epoch seconds)."""
    cycle_ms = int(anim.get("cycle_ms", 0))
    if cycle_ms <= 0:
        return 0

    now = time.time() if now is None else now
    elapsed_ms = int((now - anim.get("started_at", now)) * 1000)
    return elapsed_ms % cycle_ms


def program_info(anim, now=None):
    """The timeline a device plays, or None if nothing is playing."""
    if not anim.get("steps"):
        return None

    return {
        "position_ms": cycle_position_ms(anim, now),
        "cycle_ms": anim["cycle_ms"],
        "steps": anim["steps"],
        "clips": anim["clips"],
        "texts": anim.get("texts", []),
    }


def mono_metadata(anim, now=None):
    """Metadata for the 1-bit frames (SSD1306 and the dot matrix)."""
    frame_count = len(anim.get("frames", []))
    data = {
        "version": anim.get("version", 0),
        "frame_count": frame_count,
        **frame_timing(anim, frame_count),
    }

    program = program_info(anim, now)
    if program:
        data["program"] = program

    return data


def color_metadata(anim, now=None):
    """
    Metadata for the RGB332 frames (ST7735).

    The URL it is served from is a historical name; the data is raw RGB332,
    not JPEG. width/height/x/y describe the first GIF, for firmware that
    plays a single one.
    """
    frames = anim.get("rgb332_frames", [])
    first = (anim.get("clips") or [{}])[0]
    width = int(first.get("width", 0))
    height = int(first.get("height", 0))

    data = {
        "version": anim.get("version", 0),
        "frame_count": len(frames),
        **frame_timing(anim, len(frames)),
        "width": width,
        "height": height,
        "x": int(first.get("x", 0)),
        "y": int(first.get("y", 0)),
        "format": "rgb332",
        "frame_bytes": width * height,
    }

    program = program_info(anim, now)
    if program:
        data["program"] = program

    return data
