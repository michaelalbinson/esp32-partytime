"""
Building a device's program: the rendered frames and timeline for one lane.

A program is a list of steps played in order, then looped. A step is either a
GIF shown for a fixed time (the GIF repeats, and is cut off when time runs out)
a wait, during which the device shows its text message, an effect the device
draws itself, or text in one of a few styles. All the lane's GIFs
are rendered up front into one flat list of frames; each GIF ("clip") owns a
run of it.
"""

import base64
from collections import namedtuple

from PIL import Image

from . import config
from .media.encode import (
    composite_on_black,
    fit_geometry,
    rgb_to_mono_frame,
    rgb_to_rgb332,
)

WAIT = None  # step reference for a wait


class Effect(namedtuple("Effect", "name")):
    """Step reference for an effect the device draws itself."""


class Text(namedtuple("Text", "style text")):
    """Step reference for text the device draws in a style (see TEXT_STYLES)."""


def clean_text(text):
    """Plain printable ASCII, single-spaced, within the length limit."""
    ascii_text = "".join(
        ch if " " <= ch <= "~" else ("?" if ord(ch) > 126 else " ")
        for ch in text
    )
    return " ".join(ascii_text.split())[:config.MAX_TEXT_LENGTH]


class ProgramError(Exception):
    """The lane can't be rendered. The message is safe to show the user."""


def allocate_frames(counts, limit):
    """
    How many frames each clip keeps so the total fits in limit. Clips shrink
    in proportion, and none drops below one frame.
    """
    if len(counts) > limit:
        raise ProgramError(f"Too many different GIFs in one lane (max {limit})")

    total = sum(counts)
    if total <= limit:
        return list(counts)

    keep = [max(1, count * limit // total) for count in counts]
    while sum(keep) > limit:
        keep[keep.index(max(keep))] -= 1

    return keep


def pick_frames(count, keep):
    """Indices of the frames to keep, evenly spaced across the clip."""
    if keep >= count:
        return list(range(count))
    return [i * count // keep for i in range(keep)]


def merge_delays(delays, picked):
    """
    Delays for the picked frames. Each takes the time of the frames dropped
    after it, so the clip keeps its length.
    """
    ends = picked[1:] + [len(delays)]
    return [sum(delays[start:end]) for start, end in zip(picked, ends)]


def build_animation(steps, sources):
    """
    Render a lane. steps is a list of (gif_id, duration_ms), with gif_id WAIT
    for a wait, an Effect or a Text; sources maps gif id to ImageSource.

    Returns (animation, notes). animation holds the rendered frames and
    timeline in the shape of a device's animation record, minus the fields that
    belong to playback (version, started_at, show_id). cycle_ms is the lane's
    own length; playing a show may lengthen it.
    """
    if not steps:
        raise ProgramError("A lane needs at least one step")

    # Distinct GIFs in order of first use; repeats share one set of frames.
    clip_ids = []
    for ref, _ in steps:
        if isinstance(ref, str) and ref not in clip_ids:
            clip_ids.append(ref)

    if len(clip_ids) > config.MAX_PROGRAM_CLIPS:
        raise ProgramError(
            f"Too many different GIFs in one lane (max {config.MAX_PROGRAM_CLIPS})"
        )

    notes = []
    counts = [len(sources[gif_id].frames) for gif_id in clip_ids]
    keeps = allocate_frames(counts, config.MAX_PROGRAM_FRAMES)
    for gif_id, count, keep in zip(clip_ids, counts, keeps):
        if keep < count:
            notes.append(
                f"Reduced {gif_id} from {count} to {keep} frames to fit the device"
            )

    total_frames = max(1, sum(keeps))

    # Per-frame pixel budget: small enough that every frame of the lane fits
    # the device's RAM cache, within the hard limits.
    pixel_budget = max(
        config.ANIM_MIN_PIXELS,
        min(config.ANIM_MAX_PIXELS, config.ANIM_CACHE_BYTES // total_frames),
    )

    mono_frames = []
    rgb332_frames = []
    delays = []
    clips = []

    for gif_id, keep in zip(clip_ids, keeps):
        source = sources[gif_id]
        left, top, right, bottom = source.bbox
        crop_width = max(1, right - left)
        crop_height = max(1, bottom - top)

        width, height = fit_geometry(crop_width, crop_height, pixel_budget)
        picked = pick_frames(len(source.frames), keep)

        clips.append({
            "gif": gif_id,
            "first_frame": len(rgb332_frames),
            "frame_count": len(picked),
            "width": width,
            "height": height,
            "x": (config.SCREEN_WIDTH - width) // 2,
            "y": (config.SCREEN_HEIGHT - height) // 2,
        })
        delays.extend(merge_delays(source.delays_ms, picked))

        for index in picked:
            rgb = composite_on_black(source.frames[index], source.bbox)
            mono_frames.append(rgb_to_mono_frame(rgb))

            if rgb.size != (width, height):
                rgb = rgb.resize((width, height), Image.Resampling.LANCZOS)
            rgb332_frames.append(rgb_to_rgb332(rgb))

    texts = []
    laid_out = []
    start_ms = 0
    for ref, duration_ms in steps:
        step = {"start_ms": start_ms, "duration_ms": duration_ms}
        if ref is WAIT:
            step["clip"] = -1
        elif isinstance(ref, Effect):
            step["clip"] = -2
            step["effect"] = config.EFFECTS[ref.name]
        elif isinstance(ref, Text):
            text = clean_text(ref.text)
            if text not in texts:
                texts.append(text)
            if len(texts) > config.MAX_PROGRAM_TEXTS:
                raise ProgramError(
                    f"Too many different texts in one lane (max {config.MAX_PROGRAM_TEXTS})"
                )
            step["clip"] = -3
            step["style"] = config.TEXT_STYLES[ref.style]
            step["text"] = texts.index(text)
        else:
            step["clip"] = clip_ids.index(ref)
        laid_out.append(step)
        start_ms += duration_ms

    animation = {
        "frames": [_b64(frame) for frame in mono_frames],
        "rgb332_frames": [_b64(frame) for frame in rgb332_frames],
        "frame_delays_ms": delays,
        "clips": clips,
        "steps": laid_out,
        "texts": texts,
        "cycle_ms": start_ms,
    }
    return animation, notes


def _b64(data):
    """JSON stores frame bytes as Base64 strings."""
    return base64.b64encode(data).decode("ascii")
