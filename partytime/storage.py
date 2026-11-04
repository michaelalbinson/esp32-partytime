"""Small JSON persistence helpers shared by the stores."""

import json
import os


def load_json(path, default):
    """Return the parsed JSON at path, or default if the file doesn't exist."""
    if not os.path.exists(path):
        return default

    with open(path, "r") as f:
        return json.load(f)


def save_json(path, data):
    """Write JSON atomically so a crash can't leave a half-written file."""
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)

    tmp_path = path + ".tmp"
    with open(tmp_path, "w") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp_path, path)
