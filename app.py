"""Entry point: `python app.py` runs the server. The code lives in partytime/."""

import logging

from partytime import config, create_app

logger = logging.getLogger(__name__)

app = create_app()

if __name__ == "__main__":
    logger.info("Starting server on %s:%d", config.HOST, config.PORT)
    app.run(host=config.HOST, port=config.PORT, debug=config.DEBUG)
