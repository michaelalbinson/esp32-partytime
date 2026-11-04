"""Decoding uploaded images (GIF, PNG) into RGBA frames plus timing."""

import io
from dataclasses import dataclass

from PIL import Image, ImageSequence

from .. import config


class ImageError(Exception):
    """The upload isn't a usable image. The message is safe to show the user."""


# A still has no frame timing of its own; hold its single frame this long
# before the device checks again (nothing is redrawn while it is unchanged).
STILL_FRAME_MS = 1000

FORMATS = {"GIF": "image/gif", "PNG": "image/png"}


@dataclass
class ImageSource:
    format: str         # "GIF" or "PNG"
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


def read_image(raw, max_frames=None):
    """
    Decode a GIF or PNG (an animated PNG plays like a GIF). Raises ImageError
    with a user-facing message.
    """
    max_frames = max_frames or config.MAX_FRAMES

    try:
        image = Image.open(io.BytesIO(raw))
        if image.format not in FORMATS:
            raise ImageError("The uploaded file must be a GIF or PNG")

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
            raise ImageError("The image contains no frames")

        # A still image: hold its frame for a while rather than the 10 ms floor.
        if len(frames) == 1:
            delays_ms = [max(delays_ms[0], STILL_FRAME_MS)]

        # Fully transparent image: use a 1x1 black frame.
        if content_bbox is None:
            content_bbox = (0, 0, 1, 1)

        return ImageSource(
            format=image.format,
            frames=frames,
            delays_ms=delays_ms,
            bbox=content_bbox,
            width=image.width,
            height=image.height,
        )
    except ImageError:
        raise
    except Exception as error:
        raise ImageError("Could not read that image") from error
