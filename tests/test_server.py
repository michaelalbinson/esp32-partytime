"""Server tests. Run from the repo root: python -m unittest discover tests"""

import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from PIL import Image  # noqa: E402

from partytime import create_app  # noqa: E402
from partytime.program import allocate_frames, merge_delays, pick_frames  # noqa: E402
from partytime.protocol import cycle_position_ms  # noqa: E402
from partytime.registry import normalize_animation  # noqa: E402


def make_gif(frame_count=3, delay=100, size=(40, 30)):
    frames = [
        Image.new("RGB", size, (i * 40 % 256, 80, 200 - i % 50)).convert(
            "P", palette=Image.ADAPTIVE
        )
        for i in range(frame_count)
    ]
    buf = io.BytesIO()
    frames[0].save(buf, "GIF", save_all=True, append_images=frames[1:],
                   duration=delay, loop=0)
    return buf.getvalue()


class FrameMathTests(unittest.TestCase):
    def test_allocate_leaves_small_totals_alone(self):
        self.assertEqual(allocate_frames([10, 20], 60), [10, 20])

    def test_allocate_shrinks_in_proportion_within_limit(self):
        keep = allocate_frames([60, 30], 60)
        self.assertLessEqual(sum(keep), 60)
        self.assertEqual(keep, [40, 20])

    def test_allocate_keeps_one_frame_per_clip(self):
        keep = allocate_frames([100, 1, 1], 10)
        self.assertLessEqual(sum(keep), 10)
        self.assertTrue(all(k >= 1 for k in keep))

    def test_pick_and_merge_preserve_duration(self):
        delays = [10, 20, 30, 40, 50, 60, 70]
        picked = pick_frames(len(delays), 3)
        self.assertEqual(picked, [0, 2, 4])
        self.assertEqual(sum(merge_delays(delays, picked)), sum(delays))

    def test_cycle_position_wraps(self):
        anim = {"cycle_ms": 1000, "started_at": 100.0}
        self.assertEqual(cycle_position_ms(anim, now=100.25), 250)
        self.assertEqual(cycle_position_ms(anim, now=103.5), 500)


class LegacyMigrationTests(unittest.TestCase):
    def test_old_single_gif_record_becomes_one_looping_step(self):
        anim = normalize_animation({
            "version": 4,
            "frames": ["a", "b"],
            "color_frames": [],
            "rgb332_frames": ["x", "y"],
            "frame_delays_ms": [50, 150],
            "rgb332_width": 10, "rgb332_height": 8,
            "rgb332_x": 3, "rgb332_y": 4,
        })
        self.assertEqual(anim["clips"], [{
            "first_frame": 0, "frame_count": 2,
            "width": 10, "height": 8, "x": 3, "y": 4,
        }])
        self.assertEqual(anim["steps"], [{"clip": 0, "start_ms": 0, "duration_ms": 200}])
        self.assertEqual(anim["cycle_ms"], 200)
        self.assertNotIn("color_frames", anim)


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.client = create_app(data_dir=self.tmp.name).test_client()

        for name, screen in (("oled", "ssd1306"), ("tft", "st7735")):
            self.client.get(f"/api/message?device={name}&screen={screen}")

    def upload(self, name="a.gif", **kwargs):
        response = self.client.post(
            "/api/gifs",
            data={"gif": (io.BytesIO(make_gif(**kwargs)), name)},
            content_type="multipart/form-data",
        )
        self.assertEqual(response.status_code, 201, response.get_data(as_text=True))
        return response.get_json()

    def make_show(self, lanes, name="Party"):
        response = self.client.post("/api/shows", json={"name": name, "lanes": lanes})
        self.assertEqual(response.status_code, 201, response.get_data(as_text=True))
        return response.get_json()

    def test_library_dedupes_and_protects_used_gifs(self):
        first = self.upload("one.gif", frame_count=3)
        again = self.upload("renamed.gif", frame_count=3)
        self.assertEqual(first["id"], again["id"])
        self.assertEqual(first["name"], "one")
        self.assertEqual(len(self.client.get("/api/gifs").get_json()["gifs"]), 1)

        self.make_show([{"device": "oled", "steps": [
            {"type": "gif", "gif": first["id"], "seconds": 2}]}])
        response = self.client.delete(f"/api/gifs/{first['id']}")
        self.assertEqual(response.status_code, 409)

    def test_chase_across_displays(self):
        gif = self.upload(frame_count=4, delay=100)["id"]
        show = self.make_show([
            {"device": "oled", "steps": [
                {"type": "gif", "gif": gif, "seconds": 2},
                {"type": "wait", "seconds": 3}]},
            {"device": "tft", "steps": [
                {"type": "wait", "seconds": 2},
                {"type": "effect", "effect": "rain", "seconds": 1.5},
                {"type": "gif", "gif": gif, "seconds": 1.5}]},
        ])

        response = self.client.post(f"/api/shows/{show['id']}/play")
        self.assertEqual(response.status_code, 200, response.get_data(as_text=True))
        self.assertEqual(response.get_json()["cycle_ms"], 5000)

        oled = self.client.get("/api/animation?device=oled").get_json()
        tft = self.client.get("/api/animation/jpeg?device=tft").get_json()

        self.assertEqual(oled["program"]["cycle_ms"], 5000)
        self.assertEqual(tft["program"]["cycle_ms"], 5000)
        self.assertEqual(
            [(s["clip"], s["start_ms"], s["duration_ms"]) for s in tft["program"]["steps"]],
            [(-1, 0, 2000), (-2, 2000, 1500), (0, 3500, 1500)],
        )
        self.assertEqual(tft["program"]["steps"][1]["effect"], 1)
        self.assertEqual(tft["frame_count"], 4)
        self.assertEqual(oled["frame_count"], 4)

        # Frames are fetched by index across the whole lane.
        self.assertEqual(
            len(self.client.get("/api/animation/frame/3?device=oled").data), 1024)
        self.assertEqual(
            self.client.get("/api/animation/frame/4?device=oled").status_code, 404)

        listed = self.client.get("/api/shows").get_json()["shows"][0]
        self.assertEqual(sorted(listed["playing_on"]), ["oled", "tft"])

        # The device view names what's playing: the show started just now, so
        # oled is in its GIF step and tft is still waiting.
        devices = {d["name"]: d for d in self.client.get("/api/devices").get_json()["devices"]}
        self.assertEqual(devices["oled"]["now_playing"],
                         {"kind": "gif", "name": "a", "show": "Party"})
        self.assertEqual(devices["tft"]["now_playing"]["kind"], "wait")

        # A new message takes a device out of the show.
        self.client.post("/api/message?device=oled", json={"text": "hi"})
        self.assertEqual(self.client.get("/api/animation?device=oled").get_json()
                         .get("program"), None)
        listed = self.client.get("/api/shows").get_json()["shows"][0]
        self.assertEqual(listed["playing_on"], ["tft"])

        stopped = self.client.post(f"/api/shows/{show['id']}/stop").get_json()
        self.assertEqual(stopped["stopped"], ["tft"])

    def test_effect_only_lane_has_no_frames_but_still_plays(self):
        show = self.make_show([{"device": "tft", "steps": [
            {"type": "effect", "effect": "static", "seconds": 4}]}])
        self.assertEqual(self.client.post(f"/api/shows/{show['id']}/play").status_code, 200)

        info = self.client.get("/api/animation/jpeg?device=tft").get_json()
        self.assertEqual(info["frame_count"], 0)
        self.assertEqual(info["program"]["steps"][0]["effect"], 2)

    def test_long_lanes_are_reduced_to_fit_the_frame_limit(self):
        a = self.upload("a.gif", frame_count=40)["id"]
        b = self.upload("b.gif", frame_count=40, size=(50, 20))["id"]
        show = self.make_show([{"device": "tft", "steps": [
            {"type": "gif", "gif": a, "seconds": 4},
            {"type": "gif", "gif": b, "seconds": 4}]}])

        result = self.client.post(f"/api/shows/{show['id']}/play").get_json()
        self.assertEqual(len(result["lanes"][0]["notes"]), 2)

        info = self.client.get("/api/animation/jpeg?device=tft").get_json()
        self.assertLessEqual(info["frame_count"], 60)
        clips = info["program"]["clips"]
        self.assertEqual(clips[1]["first_frame"], clips[0]["frame_count"])

    def test_validation(self):
        gif = self.upload()["id"]
        step = {"type": "gif", "gif": gif, "seconds": 1}
        bad_bodies = [
            {"name": "", "lanes": [{"device": "oled", "steps": [step]}]},
            {"name": "x", "lanes": []},
            {"name": "x", "lanes": [{"device": "oled", "steps": []}]},
            {"name": "x", "lanes": [{"device": "oled", "steps": [step]},
                                    {"device": "oled", "steps": [step]}]},
            {"name": "x", "lanes": [{"device": "oled", "steps": [
                {"type": "gif", "gif": "nope", "seconds": 1}]}]},
            {"name": "x", "lanes": [{"device": "oled", "steps": [
                {"type": "wait", "seconds": 0}]}]},
            {"name": "x", "lanes": [{"device": "oled", "steps": [
                {"type": "effect", "effect": "fire", "seconds": 1}]}]},
        ]
        for body in bad_bodies:
            response = self.client.post("/api/shows", json=body)
            self.assertEqual(response.status_code, 400, body)

    def test_play_with_unknown_display_changes_nothing(self):
        gif = self.upload()["id"]
        show = self.make_show([
            {"device": "oled", "steps": [{"type": "gif", "gif": gif, "seconds": 1}]},
            {"device": "ghost", "steps": [{"type": "gif", "gif": gif, "seconds": 1}]},
        ])
        response = self.client.post(f"/api/shows/{show['id']}/play")
        self.assertEqual(response.status_code, 404)
        self.assertEqual(
            self.client.get("/api/animation?device=oled").get_json()["version"], 0)

    def test_state_survives_restart(self):
        gif = self.upload()["id"]
        show = self.make_show([{"device": "oled", "steps": [
            {"type": "gif", "gif": gif, "seconds": 1}]}])
        self.client.post(f"/api/shows/{show['id']}/play")

        client = create_app(data_dir=self.tmp.name).test_client()
        self.assertEqual(client.get(f"/api/shows/{show['id']}").get_json()["playing_on"],
                         ["oled"])
        self.assertIn("program", client.get("/api/animation?device=oled").get_json())

    def test_shows_page_renders(self):
        self.assertEqual(self.client.get("/shows").status_code, 200)


if __name__ == "__main__":
    unittest.main()
