"""HTTP routes, one blueprint per area."""

from . import animation, devices, gifs, messages, pages, shows

ALL = (pages, messages, animation, devices, gifs, shows)
