"""ESP32 PartyTime server: messages and GIF shows for networked displays."""

import logging
from pathlib import Path

from flask import Flask

from . import config
from .routes import ALL as ROUTES
from .services import Services

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def create_app(data_dir=None):
    """Build the app. data_dir holds devices.json, shows.json and gifs/."""
    logging.basicConfig(
        level=getattr(logging, config.LOG_LEVEL.upper()),
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )

    app = Flask(
        __name__,
        template_folder=str(PROJECT_ROOT / "templates"),
        static_folder=str(PROJECT_ROOT / "static"),
    )
    app.extensions["partytime"] = Services.open(
        data_dir if data_dir is not None else config.DATA_DIR,
        config.DEVICE_TIMEOUT_SECONDS,
    )

    for module in ROUTES:
        app.register_blueprint(module.bp)

    return app
