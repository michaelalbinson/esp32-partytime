#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <secrets.h>
#include "timeline.h"
#include "netlink.h"

#define MATRIX_HARDWARE MD_MAX72XX::FC16_HW
#define MATRIX_DEVICES 4
#define MATRIX_CS 5

#define SOURCE_WIDTH 128
#define SOURCE_HEIGHT 64
#define SOURCE_BYTES 1024

const unsigned long ANIMATION_CHECK_MS = 2000;

MD_MAX72XX matrix(MATRIX_HARDWARE, MATRIX_CS, MATRIX_DEVICES);

// What the server told us to play (see timeline.h), and our clock against it.
Timeline timeline;
TlClock clock_;
int animationVersion = -1;  // the version the drawing loop is playing

// ---- Network task ----------------------------------------------------------
// All HTTP polling runs in a task on the other core so a slow request can't
// stall drawing. The task posts what it learns into this box (netMutex guards
// it); the drawing loop takes it when it suits and owns `timeline`. The task
// never touches the matrix. (Frame downloads still happen in the drawing loop.)
SemaphoreHandle_t netMutex;
NetAnimBox animBox;

// Only the network task uses these.
Timeline netStaging;
int netVersion = -1;

// Playback: "target" is what to draw next, at changeAt (device millis). Its
// frame is fetched ahead of time, so network latency only matters if a fetch
// takes longer than the time before the change.
bool haveTarget = false;
TlPos target;
bool targetReady = false;
unsigned long changeAt = 0;
unsigned long lastFrameFetch = 0;
const unsigned long FRAME_RETRY_MS = 250;
uint8_t frameBitmap[SOURCE_BYTES];

// What is on the matrix. There is no text screen: a wait just clears it.
enum ScreenMode { SCREEN_BLANK, SCREEN_GIF, SCREEN_EFFECT, SCREEN_TEXT };
ScreenMode screenMode = SCREEN_BLANK;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;
int shownStep = -1;   // step index of the effect or text on screen (its time base)
int shownText = -1;   // index into timeline.texts
int shownStyle = TL_STYLE_NONE;
const unsigned long EFFECT_TICK_MS = 80;       // rain and static
const unsigned long EFFECT_TICK_FAST_MS = 60;  // the other effects
const unsigned long TEXT_TICK_MS = 50;
unsigned long lastEffectTick = 0;
unsigned long lastTextTick = 0;

void showMatrixFrame(const uint8_t* bitmap) {
  matrix.clear();

  // Downsample the 128x64 frame to the matrix's 32x8 pixels.
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 32; col++) {
      int sourceX = col * 4 + 2;
      int sourceY = row * 8 + 4;
      int bitIndex = sourceY * SOURCE_WIDTH + sourceX;

      bool pixelOn = bitmap[bitIndex / 8] & (0x80 >> (bitIndex % 8));
      if (pixelOn) {
        matrix.setPoint(row, col, true);
      }
    }
  }
}

// ---- Network task ------------------------------------------------------------
// Everything here may block on the network. None of it touches the matrix.

// Ask the server what to play and post the answer. Returns false if it
// couldn't be reached.
bool netPollAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;
  http.setTimeout(3000);

  unsigned long sentAt = millis();
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String response = http.getString();
  unsigned long receivedAt = millis();
  http.end();

  JSONVar info = JSON.parse(response);

  if (JSON.typeof(info) == "undefined") return false;

  int newVersion = (int)info["version"];

  if (newVersion != netVersion) {
    netVersion = newVersion;

    if (!tlLoadJson(netStaging, info)) {
      Serial.println("Bad program, ignoring it");
      tlClear(netStaging);
    }

    NetLock lock;
    netPostProgram(animBox, newVersion, netStaging);
  }

  if (tlActive(netStaging)) {
    uint32_t position = (uint32_t)(int)info["program"]["position_ms"];
    unsigned long midpoint = sentAt + (receivedAt - sentAt) / 2;

    NetLock lock;
    netPostSync(animBox, netVersion, midpoint, position, false);
  }

  return true;
}

void netTask(void *) {
  bool polledOnce = false;
  unsigned long lastCheck = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    unsigned long now = millis();

    if (!polledOnce || now - lastCheck >= ANIMATION_CHECK_MS) {
      polledOnce = true;
      lastCheck = now;
      netPollAnimation();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Download a frame into frameBitmap without drawing it.
bool fetchFrame(int index) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP +
               ":5050/api/animation/frame/" + String(index) +
               "?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;
  http.setTimeout(3000);

  int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != SOURCE_BYTES) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t received = stream->readBytes(frameBitmap, sizeof(frameBitmap));
  http.end();

  return received == sizeof(frameBitmap);
}

// ---- Effects: drawn on the matrix at its native 32x8 -----------------------

// Draw into the matrix in one go instead of sending every point.
void beginDraw() {
  matrix.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
}

void endDraw() {
  matrix.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
  matrix.update();
}

#define RAIN_COLS 32
#define RAIN_ROWS 8
int16_t rainHead[RAIN_COLS];
uint8_t rainLen[RAIN_COLS];

void startRain() {
  for (int c = 0; c < RAIN_COLS; c++) {
    rainHead[c] = -(int)(esp_random() % RAIN_ROWS);
    rainLen[c] = 2 + esp_random() % 4;
  }
}

// Digital rain: a falling streak in each column.
void tickRain() {
  beginDraw();
  matrix.clear();

  for (int c = 0; c < RAIN_COLS; c++) {
    for (int k = 0; k < rainLen[c]; k++) {
      int row = rainHead[c] - k;
      if (row >= 0 && row < RAIN_ROWS) matrix.setPoint(row, c, true);
    }

    rainHead[c]++;
    if (rainHead[c] - rainLen[c] >= RAIN_ROWS) {
      rainHead[c] = -(int)(esp_random() % 6);
      rainLen[c] = 2 + esp_random() % 4;
    }
  }

  endDraw();
}

// Static: every pixel random.
void tickStatic() {
  beginDraw();

  for (int c = 0; c < RAIN_COLS; c++) {
    uint32_t bits = esp_random();
    for (int row = 0; row < RAIN_ROWS; row++) {
      matrix.setPoint(row, c, (bits >> row) & 1);
    }
  }

  endDraw();
}

// ---- Drawing helpers ----------------------------------------------------------

// The new effects and the text draw into a 32-column buffer (bit r of fb[c] is
// row r of column c; row 0 is the top row) and push it to the matrix in one go.
uint8_t fb[RAIN_COLS];

void fbClear() {
  memset(fb, 0, sizeof(fb));
}

void px(int c, int r, bool on = true) {
  if (c < 0 || c >= RAIN_COLS || r < 0 || r >= RAIN_ROWS) return;
  if (on) fb[c] |= (uint8_t)(1 << r);
  else fb[c] &= (uint8_t)~(1 << r);
}

void fbFlush() {
  beginDraw();
  for (int c = 0; c < RAIN_COLS; c++) {
    for (int r = 0; r < RAIN_ROWS; r++) {
      matrix.setPoint(r, c, (fb[c] >> r) & 1);
    }
  }
  endDraw();
}

const float TWO_PI_F = 6.2831853f;

// Triangle wave: 0 -> 1 -> 0 over one period of x.
float tri(float x) {
  x -= floorf(x);
  return x < 0.5f ? 2.0f * x : 2.0f - 2.0f * x;
}

// ---- Text, in the library's own 5x7 font ---------------------------------------
//
// The font's column bytes have bit 0 as the top row, the same as row 0 in
// setPoint, and glyphs occupy rows 0-6. Widths vary by character.

// Fetch a character's column bytes; returns its width in columns.
uint8_t glyph(char c, uint8_t *buf) {
  if ((uint8_t)c < ' ' || c == 127) c = ' ';  // never draw control characters
  uint8_t w = matrix.getChar((uint8_t)c, 8, buf);
  if (w == 0 && c != '?') w = matrix.getChar('?', 8, buf);
  if (w == 0) {
    memset(buf, 0, 8);
    w = 3;
  }
  return w;
}

// Width in columns of len characters, with a 1 column gap between them.
int textWidth(const char *s, int len) {
  uint8_t buf[8];
  int width = 0;
  for (int i = 0; i < len; i++) {
    width += glyph(s[i], buf) + (i > 0 ? 1 : 0);
  }
  return width;
}

// Draw one character with its top-left at (x, y); returns its width. With
// on = false it clears the character's pixels instead.
int drawGlyph(char c, int x, int y, bool on = true) {
  uint8_t buf[8];
  uint8_t w = glyph(c, buf);

  for (int i = 0; i < w; i++) {
    for (int r = 0; r < 8; r++) {
      if ((buf[i] >> r) & 1) px(x + i, y + r, on);
    }
  }
  return w;
}

// Draw len characters; with wave, each bobs up and down by up to a row.
void drawText(const char *s, int len, int x, int y, bool wave, uint32_t t) {
  for (int i = 0; i < len; i++) {
    int bob = wave ? (int)roundf(sinf(t * 0.008f + i * 0.7f)) : 0;
    x += drawGlyph(s[i], x, y + bob) + 1;
  }
}

// ---- More effects, all on the 32x8 matrix --------------------------------------
// Everything that moves by time is a function of t, the time in ms since its
// step started, so every display of a show moves the same way.

// Equalizer: 32 bars rising from the bottom row.
void tickEqualizer(uint32_t t) {
  fbClear();
  for (int i = 0; i < RAIN_COLS; i++) {
    float level = (sinf(t * 0.004f * (1 + i % 3) + i * 1.7f) +
                   sinf(t * 0.0027f * (1 + i % 4) + i * 0.9f) + 2.0f) / 4.0f * RAIN_ROWS;
    int bar = (int)roundf(level);
    for (int k = 0; k < bar; k++) px(i, RAIN_ROWS - 1 - k);
  }
  fbFlush();
}

// Scanner: a two-column head sweeping end to end, a dotted trail behind it.
void tickScanner(uint32_t t) {
  float phase = (t % 1600) / 1600.0f;
  int head = (int)roundf(tri(phase) * (RAIN_COLS - 2));
  int dir = phase < 0.5f ? 1 : -1;

  fbClear();
  for (int r = 0; r < RAIN_ROWS; r++) {
    px(head, r);
    px(head + 1, r);
    if (r % 2 == 0) px(dir > 0 ? head - 1 : head + 2, r);
  }
  fbFlush();
}

// Radar: a rotating line in each 8x8 module, with two fainter lines behind it.
void tickRadar(uint32_t t) {
  float angle = TWO_PI_F * (t % 3000) / 3000.0f;

  fbClear();
  for (int m = 0; m < MATRIX_DEVICES; m++) {
    float cx = m * 8 + 3.5f;
    float cy = 3.5f;

    for (int line = 0; line < 3; line++) {
      float a = angle - line * 0.45f;
      float step = line == 0 ? 0.5f : (line == 1 ? 1.0f : 1.5f);

      for (float r = 0; r <= 3.5f; r += step) {
        px((int)floorf(cx + r * cosf(a)), (int)floorf(cy + r * sinf(a)));
      }
    }
  }
  fbFlush();
}

// Test pattern: 7 groups of columns, each a different density.
bool patternDrawn = false;

void tickTestPattern() {
  if (patternDrawn) return;
  patternDrawn = true;

  const int widths[7] = {5, 5, 5, 5, 4, 4, 4};

  fbClear();
  int c = 0;
  for (int g = 0; g < 7; g++) {
    for (int k = 0; k < widths[g]; k++, c++) {
      for (int r = 0; r < RAIN_ROWS; r++) {
        bool on = false;
        switch (g) {
          case 0: on = true; break;                  // solid
          case 1: on = (c + r) % 4 != 0; break;      // three quarters
          case 2: on = (c + r) % 2 == 0; break;      // checker
          case 3: on = (c + r) % 4 == 0; break;      // one quarter
          case 4: on = c % 2 == 0; break;            // vertical stripes
          case 5: on = r % 2 == 0; break;            // horizontal stripes
          default: on = false; break;                // empty
        }
        if (on) px(c, r);
      }
    }
  }
  fbFlush();
}

// Snow and confetti: single pixels falling and drifting sideways. These are
// random, so they differ from display to display.
#define MAX_FLAKES 10
struct Flake {
  float y;
  float vy;  // rows per tick
  int16_t x;
};
Flake flakes[MAX_FLAKES];
int flakeCount = 0;

void respawnFlake(int i, bool confetti, bool anywhere) {
  flakes[i].x = esp_random() % RAIN_COLS;
  flakes[i].y = anywhere ? (float)(esp_random() % RAIN_ROWS) : -(float)(esp_random() % 4);
  flakes[i].vy = (confetti ? 0.35f : 0.15f) + (esp_random() % 30) / 100.0f;
}

void startFlakes(bool confetti) {
  flakeCount = confetti ? 8 : 10;
  for (int i = 0; i < flakeCount; i++) respawnFlake(i, confetti, true);
}

void tickFlakes(bool confetti) {
  fbClear();
  for (int i = 0; i < flakeCount; i++) {
    flakes[i].y += flakes[i].vy;

    if (esp_random() % (confetti ? 3 : 6) == 0) {
      flakes[i].x += (esp_random() & 1) ? 1 : -1;
      flakes[i].x = (flakes[i].x + RAIN_COLS) % RAIN_COLS;
    }

    if (flakes[i].y >= RAIN_ROWS) respawnFlake(i, confetti, false);
    if (flakes[i].y >= 0) px(flakes[i].x, (int)flakes[i].y);
  }
  fbFlush();
}

// Bounce: the device name bounces off the edges (it bobs a row at each hit);
// a name wider than the matrix ping-pongs so every part is seen.
void tickBounce(uint32_t t) {
  const char *name = DEVICE_NAME;
  int len = strlen(name);
  int width = textWidth(name, len);
  int travel = abs(RAIN_COLS - width);

  float distance = t * 0.012f;  // 12 columns per second
  int x = 0;
  int hits = 0;

  if (travel > 0) {
    x = (int)roundf(travel * tri(distance / (2.0f * travel)));
    hits = (int)(distance / travel);
  }

  fbClear();
  drawText(name, len, width <= RAIN_COLS ? x : -x, hits % 2, false, t);
  fbFlush();
}

// DVD logo: the letters "DVD" bounce off the edges of the matrix. It shifts a
// row at each top/bottom hit and, for 150 ms after any hit, is drawn inverted
// (a lit block with the letters cut out) so each hit is visible. Position and
// hits are functions of t, so every display of a show agrees.
#define DVD_COLS_PER_S 10.0f
#define DVD_ROWS_PER_S 1.5f
#define DVD_HIT_FLASH_MS 150

void tickDvd(uint32_t t) {
  const char *logo = "DVD";
  int width = textWidth(logo, 3);
  int spanX = RAIN_COLS - width;
  if (spanX < 1) spanX = 1;

  float distanceX = t * DVD_COLS_PER_S / 1000.0f;
  float distanceY = t * DVD_ROWS_PER_S / 1000.0f;

  int x = (int)roundf(spanX * tri(distanceX / (2.0f * spanX)));
  int y = (int)roundf(tri(distanceY / 2.0f));  // 0 or 1

  int hitsX = (int)(distanceX / spanX);
  int hitsY = (int)distanceY;
  float msSinceX = (distanceX - hitsX * spanX) * 1000.0f / DVD_COLS_PER_S;
  float msSinceY = (distanceY - hitsY) * 1000.0f / DVD_ROWS_PER_S;
  bool flash = (hitsX > 0 && msSinceX < DVD_HIT_FLASH_MS) ||
               (hitsY > 0 && msSinceY < DVD_HIT_FLASH_MS);

  fbClear();

  if (flash) {
    for (int c = x - 1; c <= x + width; c++) {
      for (int r = y; r < y + 8; r++) px(c, r);
    }
  }

  int gx = x;
  for (int i = 0; i < 3; i++) {
    gx += drawGlyph(logo[i], gx, y, !flash) + 1;
  }

  fbFlush();
}

void tickEffect() {
  unsigned long now = millis();
  bool slow = shownEffect == TL_EFFECT_RAIN || shownEffect == TL_EFFECT_STATIC;
  if (now - lastEffectTick < (slow ? EFFECT_TICK_MS : EFFECT_TICK_FAST_MS)) return;
  lastEffectTick = now;

  uint32_t t = tlStepElapsed(timeline, clock_, now, shownStep);

  switch (shownEffect) {
    case TL_EFFECT_RAIN: tickRain(); break;
    case TL_EFFECT_STATIC: tickStatic(); break;
    case TL_EFFECT_EQUALIZER: tickEqualizer(t); break;
    case TL_EFFECT_SCANNER: tickScanner(t); break;
    case TL_EFFECT_RADAR: tickRadar(t); break;
    case TL_EFFECT_TESTPATTERN: tickTestPattern(); break;
    case TL_EFFECT_SNOW: tickFlakes(false); break;
    case TL_EFFECT_CONFETTI: tickFlakes(true); break;
    case TL_EFFECT_BOUNCE: tickBounce(t); break;
    case TL_EFFECT_DVD: tickDvd(t); break;
  }
}

// ---- Text steps ------------------------------------------------------------------

// Where the n-th word of s starts and how long it is; false if there is none.
bool nthWord(const char *s, int n, int *start, int *len) {
  int i = 0;
  while (s[i]) {
    while (s[i] == ' ') i++;
    if (!s[i]) break;

    int begin = i;
    while (s[i] && s[i] != ' ') i++;

    if (n-- == 0) {
      *start = begin;
      *len = i - begin;
      return true;
    }
  }
  return false;
}

// Marquee and wave: the line scrolls right to left, repeating, 30 columns a second.
void drawScrolling(const char *s, uint32_t t, bool wave) {
  int len = strlen(s);
  int period = textWidth(s, len) + 8;
  int offset = (int)((t * 30UL / 1000UL) % period);

  for (int x = -offset; x < RAIN_COLS; x += period) {
    drawText(s, len, x, 0, wave, t);
  }
}

// Flash words: one word at a time, centered if it fits, otherwise scrolled
// across its slot. A word's slot is 450 ms plus 40 ms per column too wide.
void drawFlashWords(const char *s, uint32_t t) {
  int start, len;
  uint32_t total = 0;

  for (int n = 0; nthWord(s, n, &start, &len); n++) {
    int excess = textWidth(s + start, len) - RAIN_COLS;
    total += 450 + (excess > 0 ? 40 * excess : 0);
  }

  if (total == 0) return;  // no words

  uint32_t at = t % total;

  for (int n = 0; nthWord(s, n, &start, &len); n++) {
    int width = textWidth(s + start, len);
    int excess = width - RAIN_COLS;
    uint32_t slot = 450 + (excess > 0 ? 40 * excess : 0);

    if (at < slot) {
      int x = excess > 0 ? -(int)((uint32_t)excess * at / slot) : (RAIN_COLS - width) / 2;
      drawText(s + start, len, x, 0, false, t);
      return;
    }
    at -= slot;
  }
}

// Copy a step's text into out (TL_TEXT_LEN bytes) with every control character,
// including newlines, as a space, runs of spaces collapsed, and no leading or
// trailing space.
void cleanText(const char *in, char *out) {
  int n = 0;
  bool pendingSpace = false;

  for (int i = 0; in[i] && n < TL_TEXT_LEN - 1; i++) {
    char c = in[i];
    if ((uint8_t)c < ' ' || c == ' ' || c == 127) {
      pendingSpace = n > 0;
      continue;
    }
    if (pendingSpace && n < TL_TEXT_LEN - 2) out[n++] = ' ';
    pendingSpace = false;
    out[n++] = c;
  }

  out[n] = '\0';
}

void tickText() {
  unsigned long now = millis();
  if (now - lastTextTick < TEXT_TICK_MS) return;
  lastTextTick = now;

  if (shownText < 0 || shownText >= timeline.textCount) return;

  // The matrix has one line: line breaks (Wavy text can carry them) and any
  // other control characters read as spaces, in runs of one.
  char s[TL_TEXT_LEN];
  cleanText(timeline.texts[shownText], s);
  uint32_t t = tlStepElapsed(timeline, clock_, now, shownStep);

  fbClear();
  if (s[0]) {
    if (shownStyle == TL_STYLE_FLASH) drawFlashWords(s, t);
    else drawScrolling(s, t, shownStyle == TL_STYLE_WAVE);
  }
  fbFlush();
}

// ---- Playback ------------------------------------------------------------------

// Put the target on the matrix. Content that is already showing is left alone.
void drawTarget() {
  if (target.kind == TL_IDLE) {
    if (screenMode != SCREEN_BLANK) {
      matrix.clear();
      screenMode = SCREEN_BLANK;
      shownFrame = -1;
      shownEffect = TL_EFFECT_NONE;
      shownText = -1;
      shownStyle = TL_STYLE_NONE;
    }
    return;
  }

  if (target.kind == TL_EFFECT) {
    // Back-to-back steps of the same effect carry on without a restart; only
    // the time base moves to the new step.
    bool changed = screenMode != SCREEN_EFFECT || shownEffect != target.effect;
    shownStep = target.step;

    if (changed) {
      screenMode = SCREEN_EFFECT;
      shownFrame = -1;
      shownEffect = target.effect;
      shownText = -1;
      shownStyle = TL_STYLE_NONE;
      lastEffectTick = 0;
      matrix.clear();

      if (shownEffect == TL_EFFECT_RAIN) startRain();
      else if (shownEffect == TL_EFFECT_SNOW) startFlakes(false);
      else if (shownEffect == TL_EFFECT_CONFETTI) startFlakes(true);
      else if (shownEffect == TL_EFFECT_TESTPATTERN) patternDrawn = false;
    }
    return;
  }

  if (target.kind == TL_TEXT) {
    if (screenMode != SCREEN_TEXT) {
      lastTextTick = 0;
    }
    screenMode = SCREEN_TEXT;
    shownFrame = -1;
    shownEffect = TL_EFFECT_NONE;
    shownStep = target.step;
    shownText = target.text;
    shownStyle = target.style;
    return;
  }

  if (screenMode == SCREEN_GIF && shownFrame == target.frame) return;

  showMatrixFrame(frameBitmap);
  screenMode = SCREEN_GIF;
  shownFrame = target.frame;
  shownEffect = TL_EFFECT_NONE;
  shownText = -1;
  shownStyle = TL_STYLE_NONE;
}

void playTimeline() {
  unsigned long now = millis();

  // Starting out, or after a re-sync: draw whatever should be showing now.
  if (!haveTarget) {
    target = tlLocate(timeline, tlPosition(timeline, clock_, now));
    changeAt = now;
    targetReady = false;
    haveTarget = true;
  }

  // Fetch the next frame as soon as we know which it is.
  if (!targetReady && now - lastFrameFetch >= FRAME_RETRY_MS) {
    lastFrameFetch = now;

    if (target.kind != TL_FRAME || (screenMode == SCREEN_GIF && shownFrame == target.frame)) {
      targetReady = true;
    } else {
      targetReady = fetchFrame(target.frame);
    }
    now = millis();
  }

  if (targetReady && (long)(now - changeAt) >= 0) {
    drawTarget();

    // The next target comes from the clock rather than the previous target, so
    // a slow fetch skips ahead instead of falling behind.
    TlPos current = tlLocate(timeline, tlPosition(timeline, clock_, now));
    changeAt = now + current.untilChangeMs;
    target = tlLocate(timeline, tlPosition(timeline, clock_, changeAt));
    targetReady = false;
    lastFrameFetch = 0;
  }

  if (screenMode == SCREEN_EFFECT) tickEffect();
  else if (screenMode == SCREEN_TEXT) tickText();
}

// Apply what the network task found (drawing loop).
void applyNetwork() {
  NetAnimTaken update;
  bool gotUpdate;
  {
    NetLock lock;
    gotUpdate = netTakeAnim(animBox, update, timeline);
  }

  if (!gotUpdate) return;

  if (update.programChanged) {
    animationVersion = update.version;
    haveTarget = false;
    targetReady = false;
    clock_.synced = false;

    // Clear whatever was playing.
    screenMode = SCREEN_BLANK;
    shownFrame = -1;
    shownEffect = TL_EFFECT_NONE;
    shownText = -1;
    shownStyle = TL_STYLE_NONE;
    matrix.clear();
  } else if (update.version != animationVersion) {
    return;  // sync for a program we haven't taken
  }

  if (update.hasSync && tlActive(timeline)) {
    if (tlSync(timeline, clock_, update.refMs, update.positionMs, update.forceSync)) {
      haveTarget = false;
      targetReady = false;
    }
  }
}

void setup() {
  Serial.begin(115200);

  // ESP32 SPI pins: SCK, MISO, MOSI, SS
  SPI.begin(18, 19, 23, MATRIX_CS);

  matrix.begin();
  matrix.control(MD_MAX72XX::INTENSITY, 2);
  matrix.clear();

  // Show device name on startup
  matrix.print(DEVICE_NAME);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());
  
  matrix.clear();
  tlClear(timeline);
  clock_.synced = false;

  // From here on the network task does the polling and reconnecting. It runs on
  // core 0 (with the WiFi stack); loop() stays on core 1 and only draws.
  netMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  applyNetwork();

  if (tlActive(timeline)) playTimeline();

  delay(10);
}
