// Timeline player shared by all three sketches.
//
// IMPORTANT: this file is copied into each sketch folder (the Arduino IDE only
// builds files inside the sketch). Edit sketch/esp-st7735/timeline.h, then run
// sketch/tests/run.sh, which checks the copies match and runs the unit tests.
//
// The server sends a "program": steps played in order, then looped. A step is a
// GIF (a run of frames, repeated until the step ends), a wait (the device shows
// its text), or an effect the device draws itself. Every display of a show gets
// the same start time, and the server also says how far into the cycle it is
// right now, so a device that knows that position can work out what to show at
// any moment from its own clock alone.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TL_MAX_STEPS 24
#define TL_MAX_CLIPS 16
#define TL_MAX_FRAMES 60
#define TL_MAX_TEXTS 8
#define TL_TEXT_LEN 104  // 100 characters plus the terminator, with spare

// TlStep.clip values below zero are not GIFs.
#define TL_CLIP_WAIT (-1)
#define TL_CLIP_EFFECT (-2)
#define TL_CLIP_TEXT (-3)

// Keep in step with EFFECTS in partytime/config.py.
#define TL_EFFECT_NONE 0
#define TL_EFFECT_RAIN 1
#define TL_EFFECT_STATIC 2
#define TL_EFFECT_EQUALIZER 3
#define TL_EFFECT_SCANNER 4
#define TL_EFFECT_RADAR 5
#define TL_EFFECT_TESTPATTERN 6
#define TL_EFFECT_SNOW 7
#define TL_EFFECT_CONFETTI 8
#define TL_EFFECT_BOUNCE 9  // the device's name bouncing around, DVD-style
#define TL_EFFECT_DVD 10    // the DVD logo bouncing around, changing color on each hit

// Keep in step with TEXT_STYLES in partytime/config.py.
#define TL_STYLE_NONE 0
#define TL_STYLE_MARQUEE 1  // one line scrolling right to left
#define TL_STYLE_FLASH 2    // one word at a time, as big as fits
#define TL_STYLE_WAVE 3     // the text filling the screen, each letter bobbing

// A device drifting further than this from the server's position is re-synced.
// Smaller differences are network jitter and are left alone so playback
// doesn't twitch every poll.
#define TL_RESYNC_MS 200

struct TlStep {
  int16_t clip;        // index into clips, or TL_CLIP_*
  uint8_t effect;      // TL_EFFECT_* when clip == TL_CLIP_EFFECT
  uint8_t style;       // TL_STYLE_* when clip == TL_CLIP_TEXT
  int8_t text;         // index into Timeline.texts when clip == TL_CLIP_TEXT
  uint32_t startMs;
  uint32_t durationMs;
};

struct TlClip {
  uint16_t firstFrame;  // index of its first frame in the lane's frame list
  uint16_t frameCount;
  uint16_t width, height, x, y;  // colour displays only
  uint32_t totalMs;              // one pass through the GIF
};

struct Timeline {
  uint32_t cycleMs;     // 0 = nothing to play
  uint8_t stepCount;
  uint8_t clipCount;
  uint16_t frameCount;
  uint8_t textCount;
  TlStep steps[TL_MAX_STEPS];
  TlClip clips[TL_MAX_CLIPS];
  uint16_t delays[TL_MAX_FRAMES];  // per-frame display time, all clips in order
  char texts[TL_MAX_TEXTS][TL_TEXT_LEN];  // plain ASCII, NUL-terminated
};

enum TlKind { TL_IDLE, TL_FRAME, TL_EFFECT, TL_TEXT };

// What should be on screen at a moment in the cycle.
struct TlPos {
  TlKind kind;
  int step;                // -1 when idle outside any step
  int clip;                // TL_FRAME only
  int frame;               // TL_FRAME only: index into the lane's frame list
  int effect;              // TL_EFFECT only
  int style;               // TL_TEXT only
  int text;                // TL_TEXT only: index into Timeline.texts
  uint32_t untilChangeMs;  // how long this stays correct; always >= 1
};

inline void tlClear(Timeline &tl) {
  tl.cycleMs = 0;
  tl.stepCount = 0;
  tl.clipCount = 0;
  tl.frameCount = 0;
  tl.textCount = 0;
}

inline bool tlActive(const Timeline &tl) {
  return tl.cycleMs > 0 && tl.stepCount > 0;
}

// What to show at time t (ms into the cycle; wrapped if beyond it).
inline TlPos tlLocate(const Timeline &tl, uint32_t t) {
  TlPos pos = {TL_IDLE, -1, -1, -1, 0, 0, -1, 1};

  if (tl.cycleMs == 0) {
    return pos;
  }

  t %= tl.cycleMs;
  uint32_t nextStart = tl.cycleMs;  // where the next step begins, for idle gaps

  for (int i = 0; i < tl.stepCount; i++) {
    const TlStep &step = tl.steps[i];
    uint32_t end = step.startMs + step.durationMs;

    if (t >= step.startMs && t < end) {
      uint32_t remaining = end - t;
      pos.step = i;
      pos.untilChangeMs = remaining;

      if (step.clip == TL_CLIP_WAIT) {
        return pos;
      }

      if (step.clip == TL_CLIP_EFFECT) {
        pos.kind = TL_EFFECT;
        pos.effect = step.effect;
        return pos;
      }

      if (step.clip == TL_CLIP_TEXT) {
        pos.kind = TL_TEXT;
        pos.style = step.style;
        pos.text = step.text;
        return pos;
      }

      const TlClip &clip = tl.clips[step.clip];
      if (clip.frameCount == 0 || clip.totalMs == 0) {
        return pos;  // nothing to draw; behave like a wait
      }

      // The GIF repeats from the start of the step.
      uint32_t offset = (t - step.startMs) % clip.totalMs;
      uint32_t elapsed = 0;
      int f = 0;

      for (; f < clip.frameCount - 1; f++) {
        uint32_t d = tl.delays[clip.firstFrame + f];
        if (offset < elapsed + d) {
          break;
        }
        elapsed += d;
      }

      uint32_t frameEnd = elapsed + tl.delays[clip.firstFrame + f];
      uint32_t untilFrame = frameEnd > offset ? frameEnd - offset : 1;

      pos.kind = TL_FRAME;
      pos.clip = step.clip;
      pos.frame = clip.firstFrame + f;
      pos.untilChangeMs = untilFrame < remaining ? untilFrame : remaining;
      return pos;
    }

    if (step.startMs > t && step.startMs < nextStart) {
      nextStart = step.startMs;
    }
  }

  // Between steps, or after the last one: idle until something starts.
  pos.untilChangeMs = nextStart - t;
  return pos;
}

// ---- Clock ------------------------------------------------------------

struct TlClock {
  bool synced;
  uint32_t startMs;  // device millis() at which the cycle was at position 0
};

inline uint32_t tlPosition(const Timeline &tl, const TlClock &clock, uint32_t nowMs) {
  return tl.cycleMs ? (uint32_t)(nowMs - clock.startMs) % tl.cycleMs : 0;
}

// How long a step has been running, in ms: the time base for effects and text,
// so that they move identically on every display of a show.
inline uint32_t tlStepElapsed(const Timeline &tl, const TlClock &clock,
                              uint32_t nowMs, int step) {
  if (tl.cycleMs == 0 || step < 0 || step >= tl.stepCount) {
    return 0;
  }

  uint32_t pos = tlPosition(tl, clock, nowMs);
  uint32_t start = tl.steps[step].startMs;
  return pos >= start ? pos - start : pos + tl.cycleMs - start;
}

// Signed distance from a to b around the cycle, in (-cycle/2, cycle/2].
inline int32_t tlCircularDiff(uint32_t cycleMs, uint32_t a, uint32_t b) {
  int32_t d = (int32_t)((b + cycleMs - a % cycleMs) % cycleMs);
  if ((uint32_t)d > cycleMs / 2) {
    d -= (int32_t)cycleMs;
  }
  return d;
}

// Adopt the server's position (positionMs, observed at device time refMs).
// Returns true if the clock changed enough that anything prefetched for the old
// timing is stale.
inline bool tlSync(const Timeline &tl, TlClock &clock, uint32_t refMs,
                   uint32_t positionMs, bool force) {
  if (tl.cycleMs == 0) {
    return false;
  }

  if (!clock.synced || force) {
    clock.startMs = refMs - positionMs;
    clock.synced = true;
    return true;
  }

  int32_t drift = tlCircularDiff(tl.cycleMs, tlPosition(tl, clock, refMs), positionMs);
  if (drift > TL_RESYNC_MS || drift < -TL_RESYNC_MS) {
    clock.startMs = refMs - positionMs;
    return true;
  }

  return false;
}

// ---- Parsing (Arduino only) ---------------------------------------------

#ifdef ARDUINO
#include <Arduino_JSON.h>

// Fill tl from the server's animation metadata. A response with no "program"
// clears it. Returns false (leaving tl cleared) if the program is malformed or
// doesn't fit the arrays.
inline bool tlLoadJson(Timeline &tl, JSONVar &info) {
  tlClear(tl);

  JSONVar program = info["program"];
  if (JSON.typeof(program) != "object") {
    return true;
  }

  JSONVar steps = program["steps"];
  JSONVar clips = program["clips"];
  JSONVar delays = info["frame_delays"];
  JSONVar texts = program["texts"];

  if (JSON.typeof(steps) != "array" || JSON.typeof(clips) != "array") {
    return false;
  }

  int textCount = (JSON.typeof(texts) == "array") ? texts.length() : 0;
  if (textCount > TL_MAX_TEXTS) {
    return false;
  }

  for (int i = 0; i < textCount; i++) {
    strncpy(tl.texts[i], (const char *)texts[i], TL_TEXT_LEN - 1);
    tl.texts[i][TL_TEXT_LEN - 1] = '\0';
  }

  int stepCount = steps.length();
  int clipCount = clips.length();
  int frameCount = (JSON.typeof(delays) == "array") ? delays.length() : 0;

  if (stepCount > TL_MAX_STEPS || clipCount > TL_MAX_CLIPS || frameCount > TL_MAX_FRAMES) {
    return false;
  }

  for (int i = 0; i < frameCount; i++) {
    int ms = (int)delays[i];
    tl.delays[i] = (ms > 0 && ms < 65535) ? ms : 100;
  }

  for (int i = 0; i < clipCount; i++) {
    JSONVar c = clips[i];
    TlClip &clip = tl.clips[i];
    clip.firstFrame = (int)c["first_frame"];
    clip.frameCount = (int)c["frame_count"];
    clip.width = (int)c["width"];
    clip.height = (int)c["height"];
    clip.x = (int)c["x"];
    clip.y = (int)c["y"];

    if ((int)clip.firstFrame + (int)clip.frameCount > frameCount) {
      return false;
    }

    clip.totalMs = 0;
    for (int f = 0; f < clip.frameCount; f++) {
      clip.totalMs += tl.delays[clip.firstFrame + f];
    }
  }

  for (int i = 0; i < stepCount; i++) {
    JSONVar s = steps[i];
    TlStep &step = tl.steps[i];
    step.clip = (int)s["clip"];
    step.effect = (JSON.typeof(s["effect"]) == "number") ? (int)s["effect"] : TL_EFFECT_NONE;
    step.style = (JSON.typeof(s["style"]) == "number") ? (int)s["style"] : TL_STYLE_NONE;
    step.text = (JSON.typeof(s["text"]) == "number") ? (int)s["text"] : -1;
    step.startMs = (uint32_t)(int)s["start_ms"];
    step.durationMs = (uint32_t)(int)s["duration_ms"];

    if (step.clip >= clipCount || step.clip < TL_CLIP_TEXT || step.durationMs == 0) {
      return false;
    }

    if (step.clip == TL_CLIP_TEXT && (step.text < 0 || step.text >= textCount)) {
      return false;
    }
  }

  tl.stepCount = stepCount;
  tl.clipCount = clipCount;
  tl.frameCount = frameCount;
  tl.textCount = textCount;
  tl.cycleMs = (uint32_t)(int)program["cycle_ms"];
  return true;
}
#endif  // ARDUINO
