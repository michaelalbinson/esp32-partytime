#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino_JSON.h>
#include <secrets.h>
#include "timeline.h"

const int OLED_SDA = 21;
const int OLED_SCL = 22;
const unsigned long POLL_INTERVAL_MS = 2000;

// What the server told us to play (see timeline.h), and our clock against it.
Timeline timeline;
TlClock clock_;
int animationVersion = -1;

// Playback: "target" is what to draw next, at changeAt (device millis). Its
// frame is fetched ahead of time, so network latency only matters if a fetch
// takes longer than the time before the change.
bool haveTarget = false;
TlPos target;
bool targetReady = false;
unsigned long changeAt = 0;
unsigned long lastFrameFetch = 0;
const unsigned long FRAME_RETRY_MS = 250;
uint8_t frameBitmap[1024];
unsigned long lastAnimationCheck = 0;

// What is on screen. Message text is only drawn in text mode, so it never
// overwrites a GIF or an effect.
enum ScreenMode { SCREEN_TEXT, SCREEN_GIF, SCREEN_EFFECT, SCREEN_STEPTEXT };
ScreenMode screenMode = SCREEN_TEXT;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;
int shownStep = -1;   // timeline step an effect or text step belongs to
int shownStyle = TL_STYLE_NONE;
int shownText = -1;
const unsigned long EFFECT_TICK_MS = 60;
unsigned long lastEffectTick = 0;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// Two-colour panels have a yellow band across the top 16 rows. While an
// animation plays, the message text dances there and the GIF fills the rest
// (the server leaves these rows black in every frame).
#define TEXT_BAND_HEIGHT 16
const unsigned long DANCE_INTERVAL_MS = 80;
const int DANCE_AMPLITUDE = 4;      // pixels up/down from the band's middle
const int SCROLL_PX_PER_TICK = 2;   // marquee speed for text wider than the screen
const int CHAR_WIDTH = 6;           // size 1 font
const int TEXT_GAP = 24;            // blank space between marquee repeats
unsigned long lastDanceTick = 0;
int dancePhase = 0;
int scrollOffset = 0;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String currentText = "";
String currentVersion = "";
bool serverOnline = false;
unsigned long lastPoll = 0;

// Ask the server what to play. Returns false if it couldn't be reached.
bool checkAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;

  unsigned long sentAt = millis();
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Animation check failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  unsigned long receivedAt = millis();
  http.end();

  JSONVar info = JSON.parse(response);

  if (JSON.typeof(info) == "undefined") {
    Serial.println("Failed to parse animation info");
    return false;
  }

  int newVersion = (int)info["version"];

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    haveTarget = false;
    targetReady = false;
    clock_.synced = false;

    if (!tlLoadJson(timeline, info)) {
      Serial.println("Bad program, ignoring it");
      tlClear(timeline);
    }

    Serial.printf("Animation updated to version %d: %d steps, %d frames, cycle %lu ms\n",
                  newVersion, timeline.stepCount, timeline.frameCount,
                  (unsigned long)timeline.cycleMs);

    // Clear whatever was playing and show the message again.
    enterTextMode();
  }

  if (tlActive(timeline)) {
    uint32_t position = (uint32_t)(int)info["program"]["position_ms"];
    unsigned long midpoint = sentAt + (receivedAt - sentAt) / 2;

    if (tlSync(timeline, clock_, midpoint, position, false)) {
      haveTarget = false;
      targetReady = false;
    }
  }

  return true;
}

// Download frame number index of the lane into frameBitmap without drawing it.
bool fetchAnimationFrame(int index) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation/frame/" + String(index) + "?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != 1024) {
    Serial.printf("Frame %d fetch failed: HTTP %d, size %d\n", index, code, http.getSize());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t received = stream->readBytes(frameBitmap, sizeof(frameBitmap));
  http.end();

  if (received != sizeof(frameBitmap)) {
    Serial.printf("Frame %d short read: %u bytes\n", index, (unsigned int)received);
    return false;
  }

  return true;
}

// Draw the message into the top band, one character at a time with each
// character bobbing on a sine wave. Text wider than the screen scrolls.
// Only touches the band, so the GIF below is left as it is in the buffer.
void drawDancingText() {
  display.fillRect(0, 0, SCREEN_WIDTH, TEXT_BAND_HEIGHT, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);

  String text = currentText.length() ? currentText : "Waiting...";
  int textWidth = text.length() * CHAR_WIDTH;
  bool scrolling = textWidth > SCREEN_WIDTH;
  int baseY = (TEXT_BAND_HEIGHT - 8) / 2;

  // Scrolling text repeats every (textWidth + gap) pixels; short text is centered.
  int startX = scrolling ? -scrollOffset : (SCREEN_WIDTH - textWidth) / 2;
  int repeatWidth = textWidth + TEXT_GAP;

  for (int copy = 0; copy < (scrolling ? 2 : 1); copy++) {
    int copyX = startX + copy * repeatWidth;
    if (copyX >= SCREEN_WIDTH || copyX + textWidth <= 0) continue;

    for (int i = 0; i < (int)text.length(); i++) {
      int x = copyX + i * CHAR_WIDTH;
      if (x < -CHAR_WIDTH || x >= SCREEN_WIDTH) continue;

      int y = baseY + (int)roundf(DANCE_AMPLITUDE * sinf((dancePhase + i * 3) * 0.2f));
      display.drawChar(x, y, text[i], SSD1306_WHITE, SSD1306_BLACK, 1);
    }
  }

  if (scrolling) scrollOffset = (scrollOffset + SCROLL_PX_PER_TICK) % repeatWidth;
  dancePhase++;
}

void drawAnimationFrame(int index) {
  display.clearDisplay();
  display.drawBitmap(0, 0, frameBitmap, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  drawDancingText();
  display.display();
  lastDanceTick = millis();
}

// Keep the text moving between GIF frames (GIFs can have long delays).
void danceTick() {
  if (millis() - lastDanceTick < DANCE_INTERVAL_MS) return;
  drawDancingText();
  display.display();
  lastDanceTick = millis();
}

void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("Server: ");
  display.println(serverOnline ? "ONLINE" : "OFFLINE");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.setTextSize(2);
  display.println(currentText.length() ? currentText : "Waiting...");
  display.display();
}

void showStartup() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Device: " + String(DEVICE_NAME));
  display.println();
  display.println("Connecting to Wi-Fi...");
  display.display();
}

// Redraw the message, but only if nothing else is on screen.
void refreshText() {
  if (screenMode == SCREEN_TEXT) drawScreen();
}

void enterTextMode() {
  screenMode = SCREEN_TEXT;
  shownFrame = -1;
  shownEffect = TL_EFFECT_NONE;
  shownStep = -1;
  shownStyle = TL_STYLE_NONE;
  shownText = -1;
  drawScreen();
}

// ---- Effects: drawn on the device at the display's full 128x64 ---------------

// Digital rain: 6x8 character cells, a bright head falling down each column
// with a trail behind it. The whole screen is redrawn every tick.
#define RAIN_COLS (SCREEN_WIDTH / 6)
#define RAIN_ROWS (SCREEN_HEIGHT / 8)
int16_t rainHead[RAIN_COLS];
uint8_t rainLen[RAIN_COLS];
uint32_t rainTick = 0;

void startRain() {
  for (int c = 0; c < RAIN_COLS; c++) {
    rainHead[c] = -(int)(esp_random() % RAIN_ROWS);
    rainLen[c] = 3 + esp_random() % 5;
  }
}

char rainGlyph(int col, int row, uint32_t salt) {
  return (char)(33 + ((col * 31 + row * 17 + salt * 7) % 94));
}

void tickRain() {
  display.clearDisplay();
  rainTick++;

  for (int c = 0; c < RAIN_COLS; c++) {
    int head = rainHead[c];

    for (int k = 0; k < rainLen[c]; k++) {
      int row = head - k;
      if (row < 0 || row >= RAIN_ROWS) continue;

      if (k == 0) {
        // The head is a white block with the glyph cut out of it.
        display.fillRect(c * 6, row * 8, 6, 8, SSD1306_WHITE);
        display.drawChar(c * 6, row * 8, rainGlyph(c, row, rainTick), SSD1306_BLACK, SSD1306_WHITE, 1);
      } else if (k < 3 || (k + row) % 2 == 0) {
        // The trail thins out so it reads as fading on a 1-bit display.
        display.drawChar(c * 6, row * 8, rainGlyph(c, row, rainTick / 4), SSD1306_WHITE, SSD1306_BLACK, 1);
      }
    }

    rainHead[c]++;
    if (rainHead[c] - rainLen[c] >= RAIN_ROWS) {
      rainHead[c] = -(int)(esp_random() % 6);
      rainLen[c] = 3 + esp_random() % 5;
    }
  }

  display.display();
}

// Static: every pixel random.
void tickStatic() {
  uint32_t *words = (uint32_t *)display.getBuffer();
  for (int i = 0; i < 1024 / 4; i++) words[i] = esp_random();
  display.display();
}

// Everything below moves as a function of the step's elapsed time (ms since the
// step began, from the show clock), so every display of a show matches. Only
// rain, static, snow and confetti keep random per-device state.

static const int EQ_BARS = 16;

float eqLevel(int bar, float t) {
  return (sinf(t * 0.004f * (1 + bar % 3) + bar * 1.7f) +
          sinf(t * 0.0027f * (1 + bar % 4) + bar * 0.9f) + 2.0f) / 4.0f;
}

// Equalizer: segmented bars growing from the bottom, with a peak cap that
// falls slowly (the highest level of the last half second, less a decay).
void tickEqualizer(uint32_t t) {
  display.clearDisplay();

  for (int i = 0; i < EQ_BARS; i++) {
    int x = i * 8;
    int h = (int)(eqLevel(i, (float)t) * 60.0f) + 2;

    for (int y = 0; y < h; y += 3) {
      display.fillRect(x, SCREEN_HEIGHT - y - 2, 7, 2, SSD1306_WHITE);
    }

    float peak = 0;
    for (int k = 0; k < 6; k++) {
      float level = eqLevel(i, (float)t - k * 100.0f) - k * 0.05f;
      if (level > peak) peak = level;
    }
    int ph = (int)(peak * 60.0f) + 4;
    if (ph > h) display.fillRect(x, SCREEN_HEIGHT - ph - 1, 7, 1, SSD1306_WHITE);
  }

  display.display();
}

// Scanner: an 8 px head sweeping across the middle, back and forth every
// 1600 ms, with a trail that thins out behind it.
void tickScanner(uint32_t t) {
  display.clearDisplay();

  uint32_t phase = t % 1600;
  bool forward = phase < 800;
  int travel = forward ? phase : 1600 - phase;
  int x = travel * (SCREEN_WIDTH - 8) / 800;
  const int y = (SCREEN_HEIGHT - 8) / 2;

  display.fillRect(x, y, 8, 8, SSD1306_WHITE);

  for (int d = 1; d <= 28; d++) {
    if (d % (1 + d / 7) != 0) continue;
    int cx = forward ? x - d : x + 7 + d;
    if (cx >= 0 && cx < SCREEN_WIDTH) display.drawFastVLine(cx, y, 8, SSD1306_WHITE);
  }

  display.display();
}

// Radar: a beam sweeping a circle every 3 s, with fading blips left behind.
static const float RADAR_BLIP_ANGLE[5] = {0.7f, 1.9f, 3.3f, 4.4f, 5.6f};
static const float RADAR_BLIP_RADIUS[5] = {20, 12, 25, 16, 22};

void dottedLine(int x0, int y0, int x1, int y1, int stride) {
  int steps = 30;
  for (int i = 0; i <= steps; i += stride) {
    display.drawPixel(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, SSD1306_WHITE);
  }
}

void tickRadar(uint32_t t) {
  const int cx = SCREEN_WIDTH / 2, cy = SCREEN_HEIGHT / 2, r = 30;
  const float TAU = 6.2831853f;

  display.clearDisplay();
  display.drawCircle(cx, cy, r, SSD1306_WHITE);
  display.drawFastHLine(cx - r, cy, 2 * r + 1, SSD1306_WHITE);
  display.drawFastVLine(cx, cy - r, 2 * r + 1, SSD1306_WHITE);

  float angle = (float)(t % 3000) / 3000.0f * TAU;

  for (int b = 0; b < 5; b++) {
    float since = fmodf(angle - RADAR_BLIP_ANGLE[b] + TAU, TAU);
    float age = since / TAU * 3000.0f;
    if (age > 1200.0f) continue;

    int bx = cx + (int)(RADAR_BLIP_RADIUS[b] * cosf(RADAR_BLIP_ANGLE[b]));
    int by = cy + (int)(RADAR_BLIP_RADIUS[b] * sinf(RADAR_BLIP_ANGLE[b]));
    int size = age < 500 ? 3 : (age < 900 ? 2 : 1);
    display.fillRect(bx - size / 2, by - size / 2, size, size, SSD1306_WHITE);
  }

  for (int k = 4; k >= 1; k--) {
    float a = angle - k * 0.12f;
    dottedLine(cx, cy, cx + (int)(r * cosf(a)), cy + (int)(r * sinf(a)), k + 1);
  }
  display.drawLine(cx, cy, cx + (int)(r * cosf(angle)), cy + (int)(r * sinf(angle)), SSD1306_WHITE);

  display.display();
}

// Test pattern: seven bars, each filled differently, over a grey ramp, with the
// device's name in the middle. Static, so it is drawn once.
static const uint8_t BAYER4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

bool barPixel(int bar, int x, int y) {
  switch (bar) {
    case 0: return true;
    case 1: return !(x % 2 == 0 && y % 2 == 0);   // 3/4
    case 2: return (x + y) % 2 == 0;              // checker
    case 3: return x % 2 == 0 && y % 2 == 0;      // 1/4
    case 4: return x % 2 == 0;                    // vertical stripes
    case 5: return y % 2 == 0;                    // horizontal stripes
    default: return false;
  }
}

void drawTestPattern() {
  const int left = 4, barW = 17, top = 3, barH = 42;

  display.clearDisplay();

  for (int bar = 0; bar < 7; bar++) {
    for (int y = top; y < top + barH; y++) {
      for (int x = left + bar * barW; x < left + (bar + 1) * barW; x++) {
        if (barPixel(bar, x, y)) display.drawPixel(x, y, SSD1306_WHITE);
      }
    }
  }

  // A ramp from black to white under the bars.
  for (int x = left; x < left + 7 * barW; x++) {
    int level = (x - left) * 16 / (7 * barW);
    for (int y = 48; y < 61; y++) {
      if (level > BAYER4[(y % 4) * 4 + (x % 4)]) display.drawPixel(x, y, SSD1306_WHITE);
    }
  }

  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  int w = strlen(DEVICE_NAME) * 6 + 8;
  if (w > SCREEN_WIDTH - 8) w = SCREEN_WIDTH - 8;
  int x = (SCREEN_WIDTH - w) / 2;
  display.fillRect(x, 25, w, 14, SSD1306_BLACK);
  display.drawRect(x, 25, w, 14, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor(x + 4, 28);
  display.print(DEVICE_NAME);

  display.display();
}

// Snow and confetti: particles falling and swaying.
#define MAX_PARTICLES 30
struct Particle {
  float x, y, v, phase;
};
Particle particles[MAX_PARTICLES];
int particleCount = 0;

void startParticles(bool confetti) {
  particleCount = confetti ? 20 : 30;

  for (int i = 0; i < particleCount; i++) {
    particles[i].x = esp_random() % SCREEN_WIDTH;
    particles[i].y = esp_random() % SCREEN_HEIGHT;
    particles[i].v = confetti ? 2.0f + (esp_random() % 200) / 100.0f
                              : 0.8f + (esp_random() % 140) / 100.0f;
    particles[i].phase = (esp_random() % 628) / 100.0f;
  }
}

void tickParticles(bool confetti) {
  display.clearDisplay();

  for (int i = 0; i < particleCount; i++) {
    Particle &p = particles[i];

    p.y += p.v;
    p.phase += confetti ? 0.3f : 0.15f;

    if (p.y > SCREEN_HEIGHT) {
      p.y = -3;
      p.x = esp_random() % SCREEN_WIDTH;
    }

    int x = (int)(p.x + (confetti ? 6 : 3) * sinf(p.phase));
    int y = (int)p.y;

    if (confetti) {
      // Tumbling: alternate between a 2x2 block and a flat 3x1 sliver.
      if (((int)(p.phase * 2)) & 1) display.fillRect(x, y, 3, 1, SSD1306_WHITE);
      else display.fillRect(x, y, 2, 2, SSD1306_WHITE);
    } else {
      int size = p.v > 1.6f ? 2 : 1;
      display.fillRect(x, y, size, size, SSD1306_WHITE);
    }
  }

  display.display();
}

// Position along a back-and-forth path, given the distance travelled so far.
int bouncePos(uint32_t travelled, int range, uint32_t *bounces) {
  if (range <= 0) {
    *bounces = 0;
    return 0;
  }

  uint32_t m = travelled % (2 * range);
  *bounces = travelled / range;
  return m <= (uint32_t)range ? (int)m : 2 * range - (int)m;
}

// Bounce: the device's name in a box, ricocheting off the edges. The box
// inverts at every wall hit.
void tickBounce(uint32_t t) {
  int len = strlen(DEVICE_NAME);
  int size = (len * 12 + 6 <= SCREEN_WIDTH) ? 2 : 1;
  int w = len * 6 * size + 6;
  if (w > SCREEN_WIDTH) w = SCREEN_WIDTH;
  int h = 8 * size + 6;

  uint32_t hitsX, hitsY;
  int x = bouncePos(t * 40 / 1000, SCREEN_WIDTH - w, &hitsX);
  int y = bouncePos(t * 25 / 1000, SCREEN_HEIGHT - h, &hitsY);
  bool inverted = (hitsX + hitsY) & 1;

  display.clearDisplay();

  if (inverted) display.fillRect(x, y, w, h, SSD1306_WHITE);
  else display.drawRect(x, y, w, h, SSD1306_WHITE);

  display.setTextSize(size);
  display.setTextWrap(false);
  display.setTextColor(inverted ? SSD1306_BLACK : SSD1306_WHITE);
  display.setCursor(x + 3, y + 3);
  display.print(DEVICE_NAME);

  display.display();
}

// ---- Text steps: a message of their own, in one of three styles --------------

// Marquee: one big line scrolling right to left, repeated so it never runs out.
void drawMarquee(const char *text, uint32_t t) {
  int len = strlen(text);
  if (len == 0) return;

  const int charW = 18;
  int textW = len * charW;
  int period = textW + 3 * charW;
  int offset = (int)((t * 60 / 1000) % period);

  for (int copy = 0; copy < 2; copy++) {
    int startX = copy * period - offset;
    if (startX >= SCREEN_WIDTH || startX + textW <= 0) continue;

    for (int i = 0; i < len; i++) {
      int x = startX + i * charW;
      if (x + charW <= 0 || x >= SCREEN_WIDTH) continue;
      display.drawChar(x, (SCREEN_HEIGHT - 24) / 2, text[i], SSD1306_WHITE, SSD1306_BLACK, 3);
    }
  }
}

// Flash words: one word at a time, as big as fits, alternating inverted.
void drawFlashWords(const char *text, uint32_t t) {
  int len = strlen(text);

  int wordCount = 0;
  for (int i = 0; i < len; i++) {
    if (text[i] != ' ' && (i == 0 || text[i - 1] == ' ')) wordCount++;
  }
  if (wordCount == 0) return;

  uint32_t slot = t / 450;
  int wanted = slot % wordCount;

  // Find the wanted word.
  int found = -1, start = 0, wordLen = 0;
  for (int i = 0; i < len; i++) {
    if (text[i] == ' ' || (i > 0 && text[i - 1] != ' ')) continue;
    found++;
    if (found == wanted) {
      start = i;
      while (start + wordLen < len && text[start + wordLen] != ' ') wordLen++;
      break;
    }
  }
  if (wordLen == 0) return;

  int size = SCREEN_WIDTH / (6 * wordLen);
  if (size > 8) size = 8;
  if (size < 1) size = 1;

  bool inverted = slot & 1;
  uint16_t fg = inverted ? SSD1306_BLACK : SSD1306_WHITE;
  uint16_t bg = inverted ? SSD1306_WHITE : SSD1306_BLACK;
  if (inverted) display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  int x = (SCREEN_WIDTH - wordLen * 6 * size) / 2;
  int y = (SCREEN_HEIGHT - 8 * size) / 2;

  for (int i = 0; i < wordLen; i++) {
    display.drawChar(x + i * 6 * size, y, text[start + i], fg, bg, size);
  }
}

#define WAVE_COLS 21
#define WAVE_MAX_LINES 5

// Split text into lines of up to WAVE_COLS characters, breaking at spaces
// where possible. Returns the number of lines.
int waveLayout(const char *text, int *starts, int *lens) {
  int len = strlen(text), pos = 0, lines = 0;

  while (pos < len && lines < WAVE_MAX_LINES) {
    while (pos < len && text[pos] == ' ') pos++;
    if (pos >= len) break;

    int take = len - pos;
    if (take > WAVE_COLS) {
      take = WAVE_COLS;
      for (int k = WAVE_COLS; k > 0; k--) {
        if (text[pos + k] == ' ') {
          take = k;
          break;
        }
      }
    }

    int end = take;
    while (end > 0 && text[pos + end - 1] == ' ') end--;

    starts[lines] = pos;
    lens[lines] = end;
    lines++;
    pos += take;
  }

  return lines;
}

// Wave: the text filling the screen, each letter bobbing on a sine wave.
void drawWaveText(const char *text, uint32_t t) {
  int starts[WAVE_MAX_LINES], lens[WAVE_MAX_LINES];
  int lines = waveLayout(text, starts, lens);
  int top = (SCREEN_HEIGHT - lines * 12) / 2 + 2;

  for (int line = 0; line < lines; line++) {
    int x0 = (SCREEN_WIDTH - lens[line] * 6) / 2;

    for (int col = 0; col < lens[line]; col++) {
      char c = text[starts[line] + col];
      if (c == ' ') continue;

      int dy = (int)roundf(2.0f * sinf(t * 0.008f + col * 0.6f + line * 0.8f));
      display.drawChar(x0 + col * 6, top + line * 12 + dy, c, SSD1306_WHITE, SSD1306_BLACK, 1);
    }
  }
}

void tickText(uint32_t t) {
  const char *text = (shownText >= 0 && shownText < timeline.textCount) ? timeline.texts[shownText] : "";

  display.clearDisplay();

  if (shownStyle == TL_STYLE_MARQUEE) drawMarquee(text, t);
  else if (shownStyle == TL_STYLE_FLASH) drawFlashWords(text, t);
  else if (shownStyle == TL_STYLE_WAVE) drawWaveText(text, t);

  display.display();
}

// Set up an effect the moment its step begins.
void startEffect(int effect) {
  if (effect == TL_EFFECT_RAIN) startRain();
  else if (effect == TL_EFFECT_SNOW) startParticles(false);
  else if (effect == TL_EFFECT_CONFETTI) startParticles(true);
  else if (effect == TL_EFFECT_TESTPATTERN) drawTestPattern();
}

void tickEffect() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  uint32_t t = tlStepElapsed(timeline, clock_, now, shownStep);

  if (screenMode == SCREEN_STEPTEXT) {
    tickText(t);
    return;
  }

  switch (shownEffect) {
    case TL_EFFECT_RAIN: tickRain(); break;
    case TL_EFFECT_STATIC: tickStatic(); break;
    case TL_EFFECT_EQUALIZER: tickEqualizer(t); break;
    case TL_EFFECT_SCANNER: tickScanner(t); break;
    case TL_EFFECT_RADAR: tickRadar(t); break;
    case TL_EFFECT_SNOW: tickParticles(false); break;
    case TL_EFFECT_CONFETTI: tickParticles(true); break;
    case TL_EFFECT_BOUNCE: tickBounce(t); break;
    default: break;  // the test pattern is static
  }
}

// ---- Playback ----------------------------------------------------------------

// Put the target on screen. Content that is already showing is left alone.
void drawTarget() {
  if (target.kind == TL_IDLE) {
    if (screenMode != SCREEN_TEXT) enterTextMode();
    return;
  }

  if (target.kind == TL_EFFECT) {
    shownStep = target.step;  // effects run on the step's own clock

    if (screenMode != SCREEN_EFFECT || shownEffect != target.effect) {
      screenMode = SCREEN_EFFECT;
      shownFrame = -1;
      shownEffect = target.effect;
      shownStyle = TL_STYLE_NONE;
      lastEffectTick = 0;
      startEffect(shownEffect);
    }
    return;
  }

  if (target.kind == TL_TEXT) {
    shownStep = target.step;
    shownText = target.text;

    if (screenMode != SCREEN_STEPTEXT || shownStyle != target.style) {
      screenMode = SCREEN_STEPTEXT;
      shownFrame = -1;
      shownEffect = TL_EFFECT_NONE;
      shownStyle = target.style;
      lastEffectTick = 0;
    }
    return;
  }

  if (screenMode == SCREEN_GIF && shownFrame == target.frame) return;

  drawAnimationFrame(target.frame);
  screenMode = SCREEN_GIF;
  shownFrame = target.frame;
  shownEffect = TL_EFFECT_NONE;
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
      targetReady = fetchAnimationFrame(target.frame);
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

  if (screenMode == SCREEN_EFFECT || screenMode == SCREEN_STEPTEXT) tickEffect();

  // Keep the message dancing between GIF frames (GIFs can have long delays).
  if (screenMode == SCREEN_GIF) danceTick();
}

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverOnline = false;
    refreshText();
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP + ":5050/api/message?device=" + DEVICE_NAME + "&screen=ssd1306";

  if (!http.begin(client, url)) {
    serverOnline = false;
    refreshText();
    return;
  }

  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String response = http.getString();
    JSONVar doc = JSON.parse(response);

    if (JSON.typeof(doc) != "undefined") {
      String newText = (const char*)doc["text"];
      String newVersion = JSON.stringify(doc["version"]);

      serverOnline = true;
      if (newText != currentText || newVersion != currentVersion) {
        currentText = newText;
        currentVersion = newVersion;
        refreshText();
      }
    } else {
      serverOnline = false;
      refreshText();
    }
  } else {
    serverOnline = false;
    refreshText();
  }

  http.end();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found at I2C address 0x3C");
    while (true) delay(1000);
  }

  showStartup();

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

  currentText = "Waiting for server";
  tlClear(timeline);
  clock_.synced = false;
  drawScreen();
}

void loop() {
  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollServer();
  }

  if (millis() - lastAnimationCheck >= 2000) {
    lastAnimationCheck = millis();
    checkAnimation();
  }

  if (tlActive(timeline)) playTimeline();

  delay(10);
}
