// Handoff between the network task and the drawing loop, shared by all three
// sketches.
//
// IMPORTANT: this file is copied into each sketch folder. Edit
// sketch/esp-st7735/netlink.h, then run sketch/tests/run.sh, which checks the
// copies match and runs the unit tests.
//
// Networking runs in its own FreeRTOS task so a slow request can't stall
// drawing. The task never touches the display or the live timeline: it parses
// what the server says and posts the result here, and the drawing loop takes it
// at a moment of its choosing. Both sides hold the lock only to copy.
//
// The boxes hold the latest result and are lossy by design: a newer post
// replaces an older one that wasn't taken, except where that would lose
// something that still matters (a new program is never dropped for a sync).

#pragma once

#include <stdint.h>
#include <string.h>

#include "timeline.h"

#define NET_MESSAGE_LEN 304  // the server allows 300 characters

// ---- Animation updates ---------------------------------------------------

struct NetAnimBox {
  bool pending;
  int version;
  bool programChanged;  // timeline is valid and replaces the current program
  Timeline timeline;
  bool hasSync;         // refMs / positionMs are valid
  uint32_t refMs;       // device millis() the server's position was observed at
  uint32_t positionMs;  // how far into the cycle the server said it was
  bool forceSync;       // adopt the position even if we are only slightly off
};

// What the drawing loop gets back (the program itself is copied to its own
// Timeline).
struct NetAnimTaken {
  int version;
  bool programChanged;
  bool hasSync;
  uint32_t refMs;
  uint32_t positionMs;
  bool forceSync;
};

// The server is on a new animation version. Replaces anything not yet taken:
// older timing belongs to the older program.
inline void netPostProgram(NetAnimBox &box, int version, const Timeline &tl) {
  box.timeline = tl;
  box.version = version;
  box.programChanged = true;
  box.hasSync = false;
  box.forceSync = false;
  box.pending = true;
}

// Where the server says the show is now. Merges into a program that hasn't been
// taken yet (they belong together) rather than replacing it.
inline void netPostSync(NetAnimBox &box, int version, uint32_t refMs,
                        uint32_t positionMs, bool force) {
  if (!box.pending) {
    box.programChanged = false;
    box.version = version;
    box.forceSync = false;
  }

  box.hasSync = true;
  box.refMs = refMs;
  box.positionMs = positionMs;
  box.forceSync = box.forceSync || force;
  box.pending = true;
}

// Move the pending update out. A program is copied into dest only if there is
// one. Returns false if nothing was pending.
inline bool netTakeAnim(NetAnimBox &box, NetAnimTaken &out, Timeline &dest) {
  if (!box.pending) {
    return false;
  }

  out.version = box.version;
  out.programChanged = box.programChanged;
  out.hasSync = box.hasSync;
  out.refMs = box.refMs;
  out.positionMs = box.positionMs;
  out.forceSync = box.forceSync;

  if (box.programChanged) {
    dest = box.timeline;
  }

  box.pending = false;
  box.programChanged = false;
  box.hasSync = false;
  box.forceSync = false;
  return true;
}

// ---- Message updates -------------------------------------------------------

struct NetMessageBox {
  bool pending;
  bool hasText;  // text/version are valid (false if the server was unreachable)
  char text[NET_MESSAGE_LEN];
  int version;
  bool online;   // the server answered
};

struct NetMessageTaken {
  bool hasText;
  char text[NET_MESSAGE_LEN];
  int version;
  bool online;
};

// text is null when the request failed; the last text posted is kept.
inline void netPostMessage(NetMessageBox &box, const char *text, int version,
                           bool online) {
  if (text) {
    strncpy(box.text, text, NET_MESSAGE_LEN - 1);
    box.text[NET_MESSAGE_LEN - 1] = '\0';
    box.version = version;
    box.hasText = true;
  } else if (!box.pending) {
    box.hasText = false;
  }

  box.online = online;
  box.pending = true;
}

inline bool netTakeMessage(NetMessageBox &box, NetMessageTaken &out) {
  if (!box.pending) {
    return false;
  }

  out.hasText = box.hasText;
  out.version = box.version;
  out.online = box.online;
  strncpy(out.text, box.text, NET_MESSAGE_LEN);
  out.text[NET_MESSAGE_LEN - 1] = '\0';

  box.pending = false;
  box.hasText = false;
  return true;
}

// ---- Locking (Arduino only) ------------------------------------------------

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The sketch defines this and creates it before starting the network task.
extern SemaphoreHandle_t netMutex;

// Holds the lock for the rest of the enclosing block.
struct NetLock {
  NetLock() { xSemaphoreTake(netMutex, portMAX_DELAY); }
  ~NetLock() { xSemaphoreGive(netMutex); }
};
#endif  // ARDUINO
