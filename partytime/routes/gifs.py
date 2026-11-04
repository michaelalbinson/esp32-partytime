"""The GIF library API."""

from flask import Blueprint, jsonify, request, send_file

from .. import config
from ..media.gif import GifError
from ..services import services

bp = Blueprint("gifs", __name__)


@bp.get("/api/gifs")
def list_gifs():
    return jsonify(gifs=services().library.list())


@bp.post("/api/gifs")
def add_gif():
    uploaded = request.files.get("gif")
    if not uploaded or not uploaded.filename:
        return jsonify(error="Choose a GIF file"), 400

    raw = uploaded.read(config.MAX_GIF_BYTES + 1)
    if len(raw) > config.MAX_GIF_BYTES:
        return jsonify(error="GIF must be 5 MB or smaller"), 413

    try:
        entry = services().library.add(raw, uploaded.filename)
    except GifError as error:
        return jsonify(error=str(error)), 400

    return jsonify(entry), 201


@bp.get("/api/gifs/<gif_id>/file")
def gif_file(gif_id):
    """The original GIF, for previews."""
    path = services().library.file_path(gif_id)
    if not path:
        return jsonify(error="GIF not found"), 404

    return send_file(path, mimetype="image/gif", max_age=3600)


@bp.delete("/api/gifs/<gif_id>")
def delete_gif(gif_id):
    services_ = services()

    if not services_.library.get(gif_id):
        return jsonify(error="GIF not found"), 404

    users = services_.shows.names_using_gif(gif_id)
    if users:
        return jsonify(
            error="GIF is used by: " + ", ".join(sorted(users))
        ), 409

    services_.library.delete(gif_id)
    return jsonify(id=gif_id, deleted=True)
