"""Decoding uploaded GIFs into RGBA frames plus timing."""

import io
from dataclasses import dataclass

from PIL import Image, ImageSequence

from .. import config


class GifError(Exception):
    """The upload isn't a usable GIF. The message is safe to show the user."""


@dataclass
class GifSource:
    frames: list        # RGBA images, at most MAX_FRAMES
    delays_ms: list     # per-frame display time
    bbox: tuple         # (left, top, right, bottom) around all visible content
    width: int          # canvas size of the original GIF
    height: int

    @property
    def duration_ms(self):
        return sum(self.delays_ms)


def gif_frame_delay_ms(frame):
    """Return a GIF frame's display time in ms, with a small minimum."""
    try:
        delay = int(frame.info.get("duration", 0))
    except (TypeError, ValueError):
        delay = 0

    return max(delay, config.MIN_GIF_DELAY_MS)


def read_gif(raw, max_frames=None):
    """Decode GIF bytes. Raises GifError with a user-facing message."""
    max_frames = max_frames or config.MAX_FRAMES

    try:
        image = Image.open(io.BytesIO(raw))
        if image.format != "GIF":
            raise GifError("The uploaded file must be a GIF")

        frames = []
        delays_ms = []
        content_bbox = None

        # Collect frames and find one bounding box around visible content
        # across the animation.
        for source_frame in ImageSequence.Iterator(image):
            if len(frames) >= max_frames:
                break

            # Read the delay before convert(), which drops frame metadata.
            delays_ms.append(gif_frame_delay_ms(source_frame))

            rgba = source_frame.convert("RGBA")
            frames.append(rgba.copy())

            bbox = rgba.getchannel("A").getbbox()
            if bbox:
                if content_bbox is None:
                    content_bbox = bbox
                else:
                    content_bbox = (
                        min(content_bbox[0], bbox[0]),
                        min(content_bbox[1], bbox[1]),
                        max(content_bbox[2], bbox[2]),
                        max(content_bbox[3], bbox[3]),
                    )

        if not frames:
            raise GifError("The GIF contains no frames")

        # Fully transparent GIF: use a 1x1 black frame.
        if content_bbox is None:
            content_bbox = (0, 0, 1, 1)

        return GifSource(
            frames=frames,
            delays_ms=delays_ms,
            bbox=content_bbox,
            width=image.width,
            height=image.height,
        )
    except GifError:
        raise
    except Exception as error:
        raise GifError("Could not read that GIF") from error
