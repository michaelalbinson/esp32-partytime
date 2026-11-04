"""Shows API: save sequences, play them across displays, stop them."""

from flask import Blueprint, jsonify, request

from ..playback import PlaybackError, play_show, playing_devices, stop_show
from ..services import services
from ..shows import ValidationError

bp = Blueprint("shows", __name__)


def _with_playback(show):
    """A show plus the devices currently playing it."""
    return {**show, "playing_on": playing_devices(services().registry, show["id"])}


@bp.get("/api/shows")
def list_shows():
    return jsonify(shows=[_with_playback(show) for show in services().shows.list()])


@bp.post("/api/shows")
def create_show():
    try:
        show = services().shows.create(request.get_json(silent=True))
    except ValidationError as error:
        return jsonify(error=str(error)), 400

    return jsonify(_with_playback(show)), 201


@bp.get("/api/shows/<show_id>")
def get_show(show_id):
    show = services().shows.get(show_id)
    if not show:
        return jsonify(error="Show not found"), 404

    return jsonify(_with_playback(show))


@bp.put("/api/shows/<show_id>")
def update_show(show_id):
    try:
        show = services().shows.update(show_id, request.get_json(silent=True))
    except ValidationError as error:
        return jsonify(error=str(error)), 400

    if not show:
        return jsonify(error="Show not found"), 404

    return jsonify(_with_playback(show))


@bp.delete("/api/shows/<show_id>")
def delete_show(show_id):
    services_ = services()

    if not services_.shows.get(show_id):
        return jsonify(error="Show not found"), 404

    stop_show(services_.registry, services_.library, show_id)
    services_.shows.delete(show_id)
    return jsonify(id=show_id, deleted=True)


@bp.post("/api/shows/<show_id>/play")
def play(show_id):
    services_ = services()

    show = services_.shows.get(show_id)
    if not show:
        return jsonify(error="Show not found"), 404

    try:
        result = play_show(services_.registry, services_.library, show)
    except PlaybackError as error:
        return jsonify(error=str(error)), error.status

    return jsonify(result)


@bp.post("/api/shows/<show_id>/stop")
def stop(show_id):
    services_ = services()

    if not services_.shows.get(show_id):
        return jsonify(error="Show not found"), 404

    stopped, restored = stop_show(services_.registry, services_.library, show_id)
    return jsonify(stopped=stopped, restored=restored)
