"""The stores a running app shares between its routes."""

import os
from dataclasses import dataclass

from flask import current_app

from .library import GifLibrary
from .registry import DeviceRegistry
from .shows import ShowStore


@dataclass
class Services:
    registry: DeviceRegistry
    library: GifLibrary
    shows: ShowStore

    @classmethod
    def open(cls, data_dir, device_timeout_seconds):
        library = GifLibrary(os.path.join(data_dir, "gifs"))
        return cls(
            registry=DeviceRegistry(
                os.path.join(data_dir, "devices.json"),
                device_timeout_seconds,
            ),
            library=library,
            shows=ShowStore(os.path.join(data_dir, "shows.json"), library),
        )


def services():
    """The current app's stores, for use inside a request."""
    return current_app.extensions["partytime"]
