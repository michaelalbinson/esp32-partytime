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
from partytime.program import allocate_frames, clean_text, merge_delays, pick_frames  # noqa: E402
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


def make_png(size=(60, 40), color=(200, 30, 30), transparent=False):
    mode = "RGBA" if transparent else "RGB"
    fill = color + (0,) if transparent else color
    image = Image.new(mode, size, fill)
    if transparent:
        for x in range(10, 50):
            for y in range(8, 32):
                image.putpixel((x, y), color + (255,))
    buf = io.BytesIO()
    image.save(buf, "PNG")
    return buf.getvalue()


def make_apng(frame_count=3, delay=80, size=(30, 20)):
    frames = [Image.new("RGB", size, (i * 70 % 256, 40, 90)) for i in range(frame_count)]
    buf = io.BytesIO()
    frames[0].save(buf, "PNG", save_all=True, append_images=frames[1:], duration=delay, loop=0)
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


class CleanTextTests(unittest.TestCase):
    def test_keeps_plain_ascii_and_squashes_whitespace(self):
        self.assertEqual(clean_text("  HAPPY   party\n time "), "HAPPY party time")

    def test_replaces_non_ascii_and_truncates(self):
        self.assertEqual(clean_text("caf\u00e9 \U0001f389"), "caf? ?")
        self.assertEqual(len(clean_text("x" * 500)), 100)


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

    def quick_upload(self, device, name="quick.gif", **kwargs):
        response = self.client.post(
            f"/api/animation?device={device}",
            data={"gif": (io.BytesIO(make_gif(**kwargs)), name)},
            content_type="multipart/form-data",
        )
        self.assertEqual(response.status_code, 200)

    def test_stop_goes_back_to_what_was_playing_before_the_show(self):
        self.quick_upload("oled", "before.gif", frame_count=5)
        # tft was only showing its message.
        show = self.make_show([
            {"device": "oled", "steps": [{"type": "effect", "effect": "rain", "seconds": 2}]},
            {"device": "tft", "steps": [{"type": "effect", "effect": "static", "seconds": 2}]},
        ])
        self.client.post(f"/api/shows/{show['id']}/play")
        self.assertEqual(self.client.get("/api/devices/oled").get_json()["now_playing"]["kind"], "effect")

        stopped = self.client.post(f"/api/shows/{show['id']}/stop").get_json()
        self.assertEqual(sorted(stopped["stopped"]), ["oled", "tft"])
        self.assertEqual(stopped["restored"], ["oled"])

        oled = self.client.get("/api/animation?device=oled").get_json()
        self.assertEqual(oled["frame_count"], 5)
        self.assertEqual(len(oled["program"]["steps"]), 1)
        self.assertEqual(self.client.get("/api/devices/oled").get_json()["now_playing"],
                         {"kind": "gif", "name": "before", "show": None})
        self.assertNotIn("program", self.client.get("/api/animation?device=tft").get_json())

        # The show no longer counts as playing anywhere.
        self.assertEqual(self.client.get(f"/api/shows/{show['id']}").get_json()["playing_on"], [])

    def test_show_over_show_returns_to_the_original_state(self):
        self.quick_upload("oled", "before.gif")
        lane = lambda effect: [{"device": "oled", "steps": [  # noqa: E731
            {"type": "effect", "effect": effect, "seconds": 2}]}]
        first = self.make_show(lane("rain"), name="A")
        second = self.make_show(lane("snow"), name="B")

        self.client.post(f"/api/shows/{first['id']}/play")
        self.client.post(f"/api/shows/{second['id']}/play")
        self.client.post(f"/api/shows/{second['id']}/stop")

        self.assertEqual(self.client.get("/api/devices/oled").get_json()["now_playing"]["name"], "before")

    def test_message_replaces_the_remembered_state(self):
        self.quick_upload("oled", "before.gif")
        show = self.make_show([{"device": "oled", "steps": [
            {"type": "effect", "effect": "rain", "seconds": 2}]}])
        self.client.post(f"/api/shows/{show['id']}/play")
        self.client.post("/api/message?device=oled", json={"text": "new"})

        stopped = self.client.post(f"/api/shows/{show['id']}/stop").get_json()
        self.assertEqual(stopped["stopped"], [])
        self.assertNotIn("program", self.client.get("/api/animation?device=oled").get_json())

    def test_stop_falls_back_to_the_message_if_the_gif_is_gone(self):
        self.quick_upload("oled", "before.gif")
        gif_id = self.client.get("/api/gifs").get_json()["gifs"][0]["id"]
        show = self.make_show([{"device": "oled", "steps": [
            {"type": "effect", "effect": "rain", "seconds": 2}]}])
        self.client.post(f"/api/shows/{show['id']}/play")
        self.client.delete(f"/api/gifs/{gif_id}")

        stopped = self.client.post(f"/api/shows/{show['id']}/stop").get_json()
        self.assertEqual(stopped["restored"], [])
        self.assertNotIn("program", self.client.get("/api/animation?device=oled").get_json())

    def test_effect_only_lane_has_no_frames_but_still_plays(self):
        show = self.make_show([{"device": "tft", "steps": [
            {"type": "effect", "effect": "static", "seconds": 4}]}])
        self.assertEqual(self.client.post(f"/api/shows/{show['id']}/play").status_code, 200)

        info = self.client.get("/api/animation/jpeg?device=tft").get_json()
        self.assertEqual(info["frame_count"], 0)
        self.assertEqual(info["program"]["steps"][0]["effect"], 2)

    def test_every_effect_is_accepted_and_numbered(self):
        from partytime import config
        steps = [{"type": "effect", "effect": name, "seconds": 1} for name in config.EFFECTS]
        show = self.make_show([{"device": "tft", "steps": steps}])
        self.assertEqual(self.client.post(f"/api/shows/{show['id']}/play").status_code, 200)

        program = self.client.get("/api/animation/jpeg?device=tft").get_json()["program"]
        self.assertEqual([s["effect"] for s in program["steps"]],
                         list(config.EFFECTS.values()))
        self.assertEqual(len(set(config.EFFECTS.values())), len(config.EFFECTS))

    def test_text_steps_carry_their_text_and_blank_means_the_message(self):
        self.client.post("/api/message?device=oled", json={"text": "Hi from oled"})
        show = self.make_show([{"device": "oled", "steps": [
            {"type": "text", "style": "marquee", "text": "PARTY TIME", "seconds": 5},
            {"type": "text", "style": "flash", "text": "", "seconds": 3},
            {"type": "text", "style": "wave", "text": "PARTY TIME", "seconds": 2}]}])
        self.assertEqual(self.client.post(f"/api/shows/{show['id']}/play").status_code, 200)

        program = self.client.get("/api/animation?device=oled").get_json()["program"]
        self.assertEqual(program["texts"], ["PARTY TIME", "Hi from oled"])
        self.assertEqual(
            [(s["clip"], s["style"], s["text"]) for s in program["steps"]],
            [(-3, 1, 0), (-3, 2, 1), (-3, 3, 0)],
        )

        playing = self.client.get("/api/devices/oled").get_json()["now_playing"]
        self.assertEqual(playing["kind"], "text")
        self.assertEqual(playing["name"], 'Marquee: "PARTY TIME"')

    def test_too_many_different_texts_is_a_clear_error(self):
        steps = [{"type": "text", "style": "flash", "text": f"word {i}", "seconds": 1}
                 for i in range(9)]
        show = self.make_show([{"device": "oled", "steps": steps}])
        response = self.client.post(f"/api/shows/{show['id']}/play")
        self.assertEqual(response.status_code, 400)
        self.assertIn("texts", response.get_json()["error"])

    def test_text_step_validation(self):
        base = {"name": "x", "lanes": [{"device": "oled", "steps": []}]}
        for step in (
            {"type": "text", "style": "spin", "text": "a", "seconds": 1},
            {"type": "text", "style": "wave", "text": 5, "seconds": 1},
            {"type": "text", "style": "wave", "text": "x" * 101, "seconds": 1},
        ):
            base["lanes"][0]["steps"] = [step]
            self.assertEqual(self.client.post("/api/shows", json=base).status_code, 400, step)

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

    def test_gif_preview_works_with_a_relative_data_dir(self):
        # The default DATA_DIR is "."; previews broke when paths stayed relative.
        previous = os.getcwd()
        os.chdir(self.tmp.name)
        self.addCleanup(os.chdir, previous)

        client = create_app(data_dir="data").test_client()
        entry = client.post(
            "/api/gifs",
            data={"gif": (io.BytesIO(make_gif()), "a.gif")},
            content_type="multipart/form-data",
        ).get_json()

        response = client.get(f"/api/gifs/{entry['id']}/file")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.mimetype, "image/gif")
        self.assertEqual(response.data[:3], b"GIF")
        response.close()

    def upload_bytes(self, data, name):
        return self.client.post(
            "/api/gifs",
            data={"gif": (io.BytesIO(data), name)},
            content_type="multipart/form-data",
        )

    def test_png_stills_work_in_shows(self):
        response = self.upload_bytes(make_png(transparent=True), "logo.png")
        self.assertEqual(response.status_code, 201, response.get_data(as_text=True))
        entry = response.get_json()
        self.assertEqual((entry["format"], entry["frame_count"], entry["name"]),
                         ("PNG", 1, "logo"))

        preview = self.client.get(f"/api/gifs/{entry['id']}/file")
        self.assertEqual((preview.status_code, preview.mimetype), (200, "image/png"))
        preview.close()

        show = self.make_show([
            {"device": "oled", "steps": [{"type": "gif", "gif": entry["id"], "seconds": 4}]},
            {"device": "tft", "steps": [
                {"type": "gif", "gif": entry["id"], "seconds": 4},
                {"type": "effect", "effect": "static", "seconds": 1}]},
        ])
        self.assertEqual(self.client.post(f"/api/shows/{show['id']}/play").status_code, 200)

        tft = self.client.get("/api/animation/jpeg?device=tft").get_json()
        self.assertEqual(tft["frame_count"], 1)
        self.assertEqual(tft["frame_delays"], [1000])
        clip = tft["program"]["clips"][0]
        # Cropped to the visible 40x24 area, and a single frame gets the whole
        # per-frame budget (27,648 px), at the same aspect ratio.
        self.assertEqual((clip["width"], clip["height"]), (214, 128))
        self.assertLessEqual(clip["width"] * clip["height"], 144 * 192)
        self.assertEqual(len(self.client.get("/api/animation/frame/0?device=oled").data), 1024)
        self.assertEqual(len(self.client.get("/api/animation/jpeg/frame/0?device=tft").data),
                         clip["width"] * clip["height"])
        self.assertEqual(self.client.get("/api/devices/oled").get_json()["now_playing"]["name"], "logo")

    def test_animated_png_plays_like_a_gif(self):
        entry = self.upload_bytes(make_apng(frame_count=3, delay=80), "spin.png").get_json()
        self.assertEqual((entry["format"], entry["frame_count"], entry["duration_ms"]),
                         ("PNG", 3, 240))

    def test_other_formats_are_refused_clearly(self):
        buf = io.BytesIO()
        Image.new("RGB", (10, 10)).save(buf, "JPEG")
        response = self.upload_bytes(buf.getvalue(), "photo.jpg")
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.get_json()["error"], "The uploaded file must be a GIF or PNG")
        self.assertEqual(self.upload_bytes(b"nope", "x.png").status_code, 400)

    def test_quick_upload_accepts_a_png(self):
        response = self.client.post(
            "/api/animation?device=oled",
            data={"gif": (io.BytesIO(make_png()), "x.png")},
            content_type="multipart/form-data",
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_json()["frame_count"], 1)

    def test_shows_page_renders(self):
        self.assertEqual(self.client.get("/shows").status_code, 200)


if __name__ == "__main__":
    unittest.main()
