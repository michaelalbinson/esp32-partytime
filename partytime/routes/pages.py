"""HTML pages and the health check."""

from datetime import datetime

from flask import Blueprint, jsonify, render_template

bp = Blueprint("pages", __name__)


@bp.get("/")
def home():
    return render_template("index.html")


@bp.get("/devices")
def devices_page():
    return render_template("devices.html")


@bp.get("/shows")
def shows_page():
    return render_template("shows.html")


@bp.get("/health")
def health():
    return jsonify(
        status="ok",
        timestamp=datetime.now().isoformat(),
    )
