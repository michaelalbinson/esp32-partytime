// Host-side tests for timeline.h. Run via sketch/tests/run.sh.
#include <stdio.h>
#include <stdlib.h>

#include "../esp-st7735/netlink.h"
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
  tl.steps[0] = {0, 0, 0, 0, 0, 2000};
  tl.steps[1] = {TL_CLIP_WAIT, 0, 0, 0, 2000, 1500};
  tl.steps[2] = {TL_CLIP_EFFECT, TL_EFFECT_RAIN, 0, 0, 3500, 500};
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

  // A text step reports its style and which text; step time is shared.
  Timeline words = makeTimeline();
  words.textCount = 1;
  strcpy(words.texts[0], "HELLO WORLD");
  words.steps[2] = {TL_CLIP_TEXT, 0, TL_STYLE_FLASH, 0, 3500, 500};
  p = tlLocate(words, 3600);
  CHECK(p.kind == TL_TEXT && p.style == TL_STYLE_FLASH && p.text == 0 && p.untilChangeMs == 400);

  TlClock zero = {true, 0};
  CHECK(tlStepElapsed(words, zero, 3600, 2) == 100);
  CHECK(tlStepElapsed(words, zero, 5000 + 3600, 2) == 100);  // next cycle
  CHECK(tlStepElapsed(words, zero, 100, 2) == 100 + 5000 - 3500);  // before it started this cycle

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

  // ---- Mailbox: the network task posts, the drawing loop takes ----------------
  static NetAnimBox box;
  static NetAnimTaken got;
  static Timeline live;
  memset(&box, 0, sizeof box);
  tlClear(live);

  CHECK(!netTakeAnim(box, got, live));  // nothing pending

  // A sync on its own comes through without touching the program.
  netPostSync(box, 3, 1000, 250, false);
  live.cycleMs = 777;
  CHECK(netTakeAnim(box, got, live));
  CHECK(!got.programChanged && got.hasSync && got.refMs == 1000 && got.positionMs == 250);
  CHECK(live.cycleMs == 777);            // not overwritten
  CHECK(!netTakeAnim(box, got, live));   // taking empties the box

  // A program followed by a sync before it is taken arrives as one update.
  Timeline fresh = makeTimeline();
  netPostProgram(box, 4, fresh);
  netPostSync(box, 4, 2000, 900, false);
  CHECK(netTakeAnim(box, got, live));
  CHECK(got.programChanged && got.version == 4 && got.hasSync && got.positionMs == 900);
  CHECK(live.cycleMs == 5000 && live.stepCount == 3);

  // A newer program replaces an untaken one, and discards its stale sync.
  Timeline other = makeTimeline();
  other.cycleMs = 1234;
  netPostProgram(box, 5, fresh);
  netPostSync(box, 5, 3000, 100, true);
  netPostProgram(box, 6, other);
  CHECK(netTakeAnim(box, got, live));
  CHECK(got.version == 6 && got.programChanged && !got.hasSync && !got.forceSync);
  CHECK(live.cycleMs == 1234);

  // Forcing a sync is sticky until taken.
  netPostSync(box, 6, 10, 20, true);
  netPostSync(box, 6, 30, 40, false);
  CHECK(netTakeAnim(box, got, live));
  CHECK(got.forceSync && got.refMs == 30 && got.positionMs == 40);

  // Messages: the latest wins; a failed poll keeps the last text.
  static NetMessageBox mbox;
  static NetMessageTaken msg;
  memset(&mbox, 0, sizeof mbox);
  CHECK(!netTakeMessage(mbox, msg));

  netPostMessage(mbox, "first", 1, true);
  netPostMessage(mbox, "second", 2, true);
  CHECK(netTakeMessage(mbox, msg));
  CHECK(msg.hasText && strcmp(msg.text, "second") == 0 && msg.version == 2 && msg.online);

  netPostMessage(mbox, nullptr, 0, false);  // server unreachable
  CHECK(netTakeMessage(mbox, msg));
  CHECK(!msg.hasText && !msg.online);

  netPostMessage(mbox, "kept", 3, true);
  netPostMessage(mbox, nullptr, 0, false);
  CHECK(netTakeMessage(mbox, msg));
  CHECK(msg.hasText && strcmp(msg.text, "kept") == 0 && !msg.online);

  char longText[400];
  memset(longText, 'x', sizeof longText);
  longText[399] = '\0';
  netPostMessage(mbox, longText, 4, true);
  CHECK(netTakeMessage(mbox, msg));
  CHECK(strlen(msg.text) == NET_MESSAGE_LEN - 1);

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("timeline tests passed\n");
  return 0;
}
