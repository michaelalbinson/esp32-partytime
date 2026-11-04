"""Turning decoded GIF frames into the pixel formats the displays use."""

from PIL import Image, ImageChops

from .. import config


def fit_geometry(crop_width, crop_height, pixel_budget):
    """
    Size a frame so it fits the display and the per-frame pixel budget, with
    the crop's aspect ratio. Unlike a fixed target rectangle, this lets wide or
    tall GIFs use more of the available pixels. Returns (width, height).
    """
    scale = min(
        config.SCREEN_WIDTH / crop_width,
        config.SCREEN_HEIGHT / crop_height,
        (pixel_budget / (crop_width * crop_height)) ** 0.5,
    )

    # Floor dimensions so the resulting frame cannot exceed the budget.
    width = max(1, int(crop_width * scale))
    height = max(1, int(crop_height * scale))

    # Guard against a floating-point rounding edge case.
    while width * height > pixel_budget:
        if width / crop_width >= height / crop_height:
            width -= 1
        else:
            height -= 1

    return width, height


def composite_on_black(rgba, bbox):
    """Crop to bbox and flatten transparency onto black."""
    cropped = rgba.crop(bbox)
    background = Image.new("RGBA", cropped.size, (0, 0, 0, 255))
    background.alpha_composite(cropped)
    return background.convert("RGB")


def rgb_to_rgb332(rgb):
    """RGB332: RRR GGG BB, one byte per pixel."""
    r, g, b = rgb.split()
    r = r.point([v & 0xE0 for v in range(256)])
    g = g.point([(v & 0xE0) >> 3 for v in range(256)])
    b = b.point([v >> 6 for v in range(256)])

    # The three fields use disjoint bits, so adding is the same as OR-ing.
    return ImageChops.add(ImageChops.add(r, g), b).tobytes()


def rgb_to_mono_frame(rgb):
    """
    Convert an RGB image to a 128x64, 1-bit-per-pixel frame for SSD1306
    devices: fit below the top text band with aspect ratio preserved, centered
    on black, dithered. The text band stays black for the device to draw on.
    Rows are packed MSB-first (what Adafruit's drawBitmap expects),
    1024 bytes in total.
    """
    area_height = config.MONO_HEIGHT - config.MONO_TEXT_BAND

    fitted = rgb.copy()
    fitted.thumbnail((config.MONO_WIDTH, area_height), Image.Resampling.LANCZOS)

    canvas = Image.new("RGB", (config.MONO_WIDTH, config.MONO_HEIGHT), (0, 0, 0))
    canvas.paste(
        fitted,
        (
            (config.MONO_WIDTH - fitted.width) // 2,
            config.MONO_TEXT_BAND + (area_height - fitted.height) // 2,
        ),
    )

    # convert("1") applies Floyd-Steinberg dithering; white pixels are 1 bits.
    return canvas.convert("L").convert("1").tobytes()
