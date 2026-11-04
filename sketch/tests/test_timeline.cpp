// Host-side tests for timeline.h. Run via sketch/tests/run.sh.
#include <stdio.h>
#include <stdlib.h>

#include "../esp-st7735/timeline.h"

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
      failures++;                                                      \
    }                                                                  \
  } while (0)

// The chase from the server test: one GIF of 4 frames x 100 ms (400 ms/pass).
//   0-2000   GIF     2000-3500 wait     3500-4000 rain     (cycle 5000)
static Timeline makeTimeline() {
  Timeline tl;
  tlClear(tl);
  tl.cycleMs = 5000;
  tl.frameCount = 4;
  for (int i = 0; i < 4; i++) tl.delays[i] = 100;

  tl.clipCount = 1;
  tl.clips[0] = {0, 4, 40, 30, 100, 145, 400};

  tl.stepCount = 3;
  tl.steps[0] = {0, 0, 0, 2000};
  tl.steps[1] = {TL_CLIP_WAIT, 0, 2000, 1500};
  tl.steps[2] = {TL_CLIP_EFFECT, TL_EFFECT_RAIN, 3500, 500};
  return tl;
}

int main() {
  Timeline tl = makeTimeline();

  // GIF frames, repeating every 400 ms.
  TlPos p = tlLocate(tl, 0);
  CHECK(p.kind == TL_FRAME && p.frame == 0 && p.untilChangeMs == 100);
  p = tlLocate(tl, 250);
  CHECK(p.kind == TL_FRAME && p.frame == 2 && p.untilChangeMs == 50);
  p = tlLocate(tl, 450);  // second pass
  CHECK(p.kind == TL_FRAME && p.frame == 0 && p.untilChangeMs == 50);

  // The step ends mid-frame: the change comes at the step boundary.
  p = tlLocate(tl, 1990);
  CHECK(p.kind == TL_FRAME && p.untilChangeMs == 10);

  // Wait, effect, and the idle tail up to the end of the cycle.
  p = tlLocate(tl, 2000);
  CHECK(p.kind == TL_IDLE && p.step == 1 && p.untilChangeMs == 1500);
  p = tlLocate(tl, 3500);
  CHECK(p.kind == TL_EFFECT && p.effect == TL_EFFECT_RAIN && p.untilChangeMs == 500);
  p = tlLocate(tl, 4200);
  CHECK(p.kind == TL_IDLE && p.step == -1 && p.untilChangeMs == 800);

  // Wraps at the cycle length.
  p = tlLocate(tl, 5000 + 250);
  CHECK(p.kind == TL_FRAME && p.frame == 2);

  // A gap between steps idles until the next step starts.
  Timeline gap = makeTimeline();
  gap.steps[1].startMs = 2500;
  p = tlLocate(gap, 2100);
  CHECK(p.kind == TL_IDLE && p.step == -1 && p.untilChangeMs == 400);

  // Following untilChangeMs always moves forward and reaches every boundary.
  uint32_t t = 0;
  int changes = 0;
  while (t < 5000) {
    p = tlLocate(tl, t);
    CHECK(p.untilChangeMs >= 1);
    t += p.untilChangeMs;
    changes++;
  }
  CHECK(t == 5000);
  CHECK(changes == 20 /*gif*/ + 1 /*wait*/ + 1 /*rain*/ + 1 /*idle*/);

  // Nothing to play.
  Timeline empty;
  tlClear(empty);
  CHECK(!tlActive(empty));
  CHECK(tlLocate(empty, 123).kind == TL_IDLE);

  // Circular differences.
  CHECK(tlCircularDiff(5000, 100, 300) == 200);
  CHECK(tlCircularDiff(5000, 4900, 100) == 200);
  CHECK(tlCircularDiff(5000, 100, 4900) == -200);

  // Clock: first sync adopts; small drift is ignored; large drift re-syncs;
  // the cycle wrapping is not drift.
  TlClock clock = {false, 0};
  CHECK(tlSync(tl, clock, 10000, 1000, false));
  CHECK(tlPosition(tl, clock, 10000) == 1000);
  CHECK(tlPosition(tl, clock, 10500) == 1500);

  CHECK(!tlSync(tl, clock, 20000, (1000 + 10000 + 120) % 5000, false));  // 120 ms off
  CHECK(tlSync(tl, clock, 20000, (1000 + 10000 + 400) % 5000, false));   // 400 ms off
  CHECK(tlPosition(tl, clock, 20000) == (1000 + 10000 + 400) % 5000);

  CHECK(!tlSync(tl, clock, 30000, tlPosition(tl, clock, 30000) % 5000, false));
  CHECK(tlSync(tl, clock, 30000, 10, true));  // forced
  CHECK(tlPosition(tl, clock, 30000) == 10);

  // Clock survives millis() wrapping.
  TlClock wrap = {false, 0};
  tlSync(tl, wrap, 0xFFFFFF00u, 100, true);
  CHECK(tlPosition(tl, wrap, 0xFFFFFF00u + 300) == 400);

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("timeline tests passed\n");
  return 0;
}
