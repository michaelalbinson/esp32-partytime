"""The GIF library: uploaded GIFs kept on disk so shows can reuse them."""

import hashlib
import os
import time
from threading import RLock

from . import config
from .media.gif import GifError, read_gif
from .storage import load_json, save_json


class GifLibrary:
    """
    Raw GIF files plus an index of their metadata. GIFs are stored once
    (identified by a hash of their bytes) and rendered per device when a show
    plays, because the right frame size depends on the lane they're in.
    """

    def __init__(self, directory):
        self.directory = directory
        self.index_path = os.path.join(directory, "index.json")
        self.lock = RLock()
        self.entries = load_json(self.index_path, {})

    def _gif_path(self, gif_id):
        return os.path.join(self.directory, f"{gif_id}.gif")

    def list(self):
        """All GIFs, newest first."""
        with self.lock:
            return sorted(
                self.entries.values(),
                key=lambda entry: entry["added_at"],
                reverse=True,
            )

    def get(self, gif_id):
        with self.lock:
            return self.entries.get(gif_id)

    def file_path(self, gif_id):
        """Path to the stored GIF, or None if it isn't in the library."""
        with self.lock:
            return self._gif_path(gif_id) if gif_id in self.entries else None

    def add(self, raw, filename, source=None):
        """
        Store a GIF and return its entry. Adding the same bytes again returns
        the existing entry. Raises GifError if it isn't a usable GIF.
        """
        gif_id = hashlib.sha1(raw).hexdigest()[:12]

        with self.lock:
            if gif_id in self.entries:
                return self.entries[gif_id]

        source = source or read_gif(raw)

        name = os.path.splitext(os.path.basename(filename or ""))[0].strip()
        entry = {
            "id": gif_id,
            "name": name[:config.MAX_NAME_LENGTH] or "GIF",
            "size": len(raw),
            "frame_count": len(source.frames),
            "duration_ms": source.duration_ms,
            "width": source.width,
            "height": source.height,
            "added_at": time.time(),
        }

        with self.lock:
            os.makedirs(self.directory, exist_ok=True)
            with open(self._gif_path(gif_id), "wb") as f:
                f.write(raw)

            self.entries[gif_id] = entry
            save_json(self.index_path, self.entries)

        return entry

    def load_source(self, gif_id):
        """Decode a stored GIF. Raises GifError if it is missing or unreadable."""
        path = self.file_path(gif_id)
        if not path or not os.path.exists(path):
            raise GifError(f"GIF {gif_id} is not in the library")

        with open(path, "rb") as f:
            return read_gif(f.read())

    def delete(self, gif_id):
        """Remove a GIF. Returns False if it wasn't there."""
        with self.lock:
            if gif_id not in self.entries:
                return False

            del self.entries[gif_id]
            save_json(self.index_path, self.entries)

            try:
                os.remove(self._gif_path(gif_id))
            except FileNotFoundError:
                pass

        return True
