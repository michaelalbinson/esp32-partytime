#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino_JSON.h>
#include <secrets.h>
#include "timeline.h"

// Display pins
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 23
#define TFT_SCLK 18

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

// Maximum RGB332 frame capacity: 144 * 192 pixels.
// A GIF's frames may have any dimensions, provided their pixel count fits this
// buffer and their position/dimensions fit the display.
#define MAX_FRAME_PIXELS (144 * 192)

// RGB565 conversion buffer. This is wide enough for any strip on the screen.
#define STRIP_ROWS 16
#define MAX_STRIP_PIXELS (SCREEN_WIDTH * STRIP_ROWS)

const unsigned long POLL_INTERVAL_MS = 2000;

Adafruit_ST7789 display(TFT_CS, TFT_DC, TFT_RST);

// What the server told us to play (see timeline.h), and our clock against it.
Timeline timeline;
TlClock clock_;
int animationVersion = -1;

// Frames are stored as RGB332, one byte per pixel, in malloc'd buffers (see the
// frame cache below). There is no static frame buffer, so that RAM stays free.
//
// Frames are cached in RAM after their first download so playback doesn't wait
// on the network. Each frame gets its own malloc'd slot (no big contiguous block
// needed). Caching stops at the byte budget, or when free heap would drop below
// the reserve kept for WiFi/HTTP/JSON; any frames that don't fit stream through
// streamBuffer, which is only allocated if that happens. The server sizes
// frames so a whole lane fits (ANIM_CACHE_BYTES in partytime/config.py matches
// CACHE_BUDGET_BYTES). The cache is indexed by the lane's frame number.
#define MAX_CACHE_FRAMES TL_MAX_FRAMES
#define CACHE_BUDGET_BYTES (120 * 1024)
#define HEAP_RESERVE_BYTES (55 * 1024)
uint8_t *frameCache[MAX_CACHE_FRAMES];
size_t cacheBytes = 0;
int cachedFrames = 0;
uint8_t *streamBuffer = nullptr;
size_t streamBufferSize = 0;
// Buffer holding the frame that is ready to draw (a cache slot or streamBuffer).
const uint8_t *readyFrame = nullptr;
bool readyFromCache = false;

// Reusable buffer for converting and drawing a strip as RGB565.
uint16_t pixelBuffer[MAX_STRIP_PIXELS];

String currentText = "";
bool serverOnline = false;
unsigned long lastPoll = 0;

// What is on screen. The message text is redrawn by pollServer() only while the
// screen is in text mode, so it never overwrites a GIF or an effect.
enum ScreenMode { SCREEN_TEXT, SCREEN_GIF, SCREEN_EFFECT };
ScreenMode screenMode = SCREEN_TEXT;
int shownClip = -1;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;

// Playback: "target" is what to draw next, at changeAt (device millis). Its
// frame is fetched ahead of time, so network latency only matters if a fetch
// takes longer than the time before the change.
bool haveTarget = false;
TlPos target;
bool targetReady = false;
unsigned long changeAt = 0;
unsigned long lastFrameFetch = 0;
const unsigned long FRAME_RETRY_MS = 250;
bool needPreload = false;

// Timing/heap stats for the last drawn frame, logged when it is drawn.
unsigned long lastFetchMs = 0;

// ---- Text screen ---------------------------------------------------------

void drawTextScreen() {
  if (screenMode != SCREEN_TEXT) {
    return;
  }

  // Clear the previous message/display contents before drawing the current one.
  display.fillScreen(ST77XX_BLACK);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setTextWrap(true);

  display.setCursor(8, 12);
  display.print("Server: ");
  display.println(serverOnline ? "ONLINE" : "OFFLINE");

  display.drawLine(8, 42, SCREEN_WIDTH - 8, 42, ST77XX_WHITE);

  display.setCursor(8, 60);
  display.println(currentText.length() ? currentText : "Waiting...");
}

void enterTextMode() {
  screenMode = SCREEN_TEXT;
  shownClip = -1;
  shownFrame = -1;
  shownEffect = TL_EFFECT_NONE;
  drawTextScreen();
}

void showStartup() {
  display.fillScreen(ST77XX_BLACK);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setTextWrap(true);
  display.setCursor(8, 12);
  display.println("Device: " + String(DEVICE_NAME));
  display.println();
  display.println("Connecting to Wi-Fi...");
}

// ---- Frame cache ---------------------------------------------------------

// Release every cached frame (the animation or its geometry changed).
void freeFrameCache() {
  for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
    if (frameCache[i]) {
      free(frameCache[i]);
      frameCache[i] = nullptr;
    }
  }
  cacheBytes = 0;
  cachedFrames = 0;
  readyFrame = nullptr;

  if (streamBuffer) {
    free(streamBuffer);
    streamBuffer = nullptr;
    streamBufferSize = 0;
  }
}

// Scratch buffer for frames that didn't fit in the cache.
uint8_t *getStreamBuffer(size_t size) {
  if (streamBuffer && streamBufferSize >= size) return streamBuffer;

  if (streamBuffer) free(streamBuffer);
  streamBuffer = (uint8_t *)malloc(size);
  streamBufferSize = streamBuffer ? size : 0;
  return streamBuffer;
}

// Try to reserve a cache slot for a frame; returns nullptr if over budget or
// if it would eat into the heap reserve.
uint8_t *allocCacheSlot(int index, size_t size) {
  if (index < 0 || index >= MAX_CACHE_FRAMES) return nullptr;
  if (frameCache[index]) return nullptr;
  if (cacheBytes + size > CACHE_BUDGET_BYTES) return nullptr;
  if (ESP.getFreeHeap() < size + HEAP_RESERVE_BYTES) return nullptr;

  uint8_t *slot = (uint8_t *)malloc(size);
  if (!slot) return nullptr;

  frameCache[index] = slot;
  cacheBytes += size;
  cachedFrames++;
  return slot;
}

void releaseCacheSlot(int index, size_t size) {
  if (index < 0 || index >= MAX_CACHE_FRAMES || !frameCache[index]) return;
  free(frameCache[index]);
  frameCache[index] = nullptr;
  cacheBytes -= size;
  cachedFrames--;
}

// The clip that owns a frame of the lane's frame list.
int clipOfFrame(int frame) {
  for (int i = 0; i < timeline.clipCount; i++) {
    const TlClip &c = timeline.clips[i];
    if (frame >= c.firstFrame && frame < c.firstFrame + c.frameCount) return i;
  }
  return -1;
}

// ---- Network ---------------------------------------------------------------

bool readExact(WiFiClient *stream, uint8_t *buffer, size_t length) {
  size_t received = 0;
  unsigned long started = millis();

  while (received < length && millis() - started < 10000) {
    int availableBytes = stream->available();

    if (availableBytes > 0) {
      size_t remaining = length - received;
      size_t toRead = (size_t)availableBytes;

      if (toRead > remaining) {
        toRead = remaining;
      }

      size_t count = stream->readBytes(buffer + received, toRead);
      received += count;
    } else {
      if (!stream->connected()) {
        break;
      }
      delay(1);
    }
  }

  return received == length;
}

// Download frame number frameIndex of the lane (pixelCount bytes) into dest
// without drawing it.
bool fetchFrame(int frameIndex, uint8_t *dest, size_t pixelCount) {
  if (WiFi.status() != WL_CONNECTED || pixelCount == 0 ||
      pixelCount > MAX_FRAME_PIXELS) {
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP +
               ":5050/api/animation/jpeg/frame/" + String(frameIndex) +
               "?device=" + DEVICE_NAME;

  http.setTimeout(10000);

  if (!http.begin(client, url)) {
    Serial.println("Frame request: could not start request");
    return false;
  }

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Frame %d request: HTTP %d\n", frameIndex, code);
    http.end();
    return false;
  }

  int contentLength = http.getSize();

  if (contentLength >= 0 && (size_t)contentLength != pixelCount) {
    Serial.printf("Frame %d has unexpected size: %d; expected %u bytes\n",
                  frameIndex,
                  contentLength,
                  (unsigned int)pixelCount);
    http.end();
    return false;
  }

  // Receive the whole RGB332 frame before drawing any part of it.
  WiFiClient *stream = http.getStreamPtr();

  if (!readExact(stream, dest, pixelCount)) {
    Serial.printf("Frame %d: failed reading full frame\n", frameIndex);
    http.end();
    return false;
  }

  http.end();
  return true;
}

// Check the clips fit the pixel budget and the display.
bool validateGeometry() {
  for (int i = 0; i < timeline.clipCount; i++) {
    const TlClip &c = timeline.clips[i];
    size_t pixels = (size_t)c.width * (size_t)c.height;

    if (c.frameCount > 0 &&
        (c.width < 1 || c.height < 1 || pixels > MAX_FRAME_PIXELS ||
         c.x + c.width > SCREEN_WIDTH || c.y + c.height > SCREEN_HEIGHT)) {
      Serial.printf("Invalid frame geometry: %dx%d at %d,%d\n",
                    c.width, c.height, c.x, c.y);
      return false;
    }
  }
  return true;
}

// Stop playing and forget what was prefetched or on screen.
void resetPlayback() {
  haveTarget = false;
  targetReady = false;
  readyFrame = nullptr;
  lastFrameFetch = 0;
}

// Ask the server what to play. forceSync adopts the server's position even if
// we're only slightly off. Returns false if the server couldn't be reached.
bool checkAnimation(bool forceSync = false) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP +
               ":5050/api/animation/jpeg?device=" + DEVICE_NAME;

  http.setTimeout(5000);

  if (!http.begin(client, url)) {
    Serial.println("Animation check: could not start request");
    return false;
  }

  unsigned long sentAt = millis();
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Animation check: HTTP %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  unsigned long receivedAt = millis();
  http.end();

  JSONVar info = JSON.parse(response);

  if (JSON.typeof(info) == "undefined") {
    Serial.println("Animation check: invalid JSON");
    return false;
  }

  int newVersion = (int)info["version"];

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    freeFrameCache();
    resetPlayback();
    clock_.synced = false;

    if (!tlLoadJson(timeline, info) || !validateGeometry()) {
      Serial.println("Animation check: bad program, ignoring it");
      tlClear(timeline);
    }

    Serial.printf("Animation changed: version %d, %d steps, %d frames, cycle %lu ms\n",
                  newVersion, timeline.stepCount, timeline.frameCount,
                  (unsigned long)timeline.cycleMs);

    // Clear the previous GIF, effect or message; text mode redraws the message.
    screenMode = SCREEN_TEXT;
    enterTextMode();
    needPreload = tlActive(timeline) && timeline.frameCount > 0;
  }

  if (tlActive(timeline)) {
    JSONVar program = info["program"];
    uint32_t position = (uint32_t)(int)program["position_ms"];

    // The server stamped the position somewhere between our request and its
    // answer; the midpoint is the best guess for when.
    unsigned long midpoint = sentAt + (receivedAt - sentAt) / 2;

    if (tlSync(timeline, clock_, midpoint, position, forceSync)) {
      resetPlayback();
    }
  }

  return true;
}

// Download every frame into the cache before starting, so playback never waits
// on WiFi. Frames that don't fit the cache are streamed later.
void preloadFrames() {
  for (int f = 0; f < timeline.frameCount; f++) {
    int clipIndex = clipOfFrame(f);
    if (clipIndex < 0) continue;

    const TlClip &c = timeline.clips[clipIndex];
    size_t pixels = (size_t)c.width * (size_t)c.height;
    uint8_t *slot = allocCacheSlot(f, pixels);

    if (!slot) {
      break;  // cache full; the rest streams
    }

    display.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    display.setTextSize(2);
    display.setCursor(8, 12);
    display.printf("Loading %d/%d  ", f + 1, (int)timeline.frameCount);

    if (!fetchFrame(f, slot, pixels)) {
      releaseCacheSlot(f, pixels);
      break;
    }
  }

  Serial.printf("Preloaded %d of %d frames (%u bytes)\n",
                cachedFrames, (int)timeline.frameCount, (unsigned int)cacheBytes);
  drawTextScreen();
}

// Ask the server again after something slow (the preload) so the clock is
// current, then forget any plan made against the old one.
void refreshAnimation() {
  if (!checkAnimation()) {
    return;
  }

  if (needPreload) {
    needPreload = false;
    preloadFrames();
    checkAnimation(true);
  }
}

// ---- Drawing ---------------------------------------------------------------

// Convert a frame and draw it in RGB565 strips.
bool drawFrame(const uint8_t *src, const TlClip &clip) {
  for (int row = 0; row < clip.height; row += STRIP_ROWS) {
    int rows = STRIP_ROWS;

    if (row + rows > clip.height) {
      rows = clip.height - row;
    }

    size_t stripPixels = (size_t)clip.width * (size_t)rows;

    if (stripPixels > MAX_STRIP_PIXELS) {
      Serial.println("Strip exceeds RGB565 buffer");
      return false;
    }

    size_t sourceOffset = (size_t)row * (size_t)clip.width;

    for (size_t i = 0; i < stripPixels; i++) {
      uint8_t c = src[sourceOffset + i];

      uint8_t r3 = (c >> 5) & 0x07;
      uint8_t g3 = (c >> 2) & 0x07;
      uint8_t b2 = c & 0x03;

      uint8_t r5 = (r3 << 2) | (r3 >> 1);
      uint8_t g6 = (g3 << 3) | g3;
      uint8_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);

      pixelBuffer[i] = ((uint16_t)r5 << 11) |
                       ((uint16_t)g6 << 5) |
                       b5;
    }

    display.startWrite();
    display.setAddrWindow(clip.x, clip.y + row, clip.width, rows);
    display.writePixels(
      pixelBuffer,
      (uint32_t)stripPixels,
      true,
      false
    );
    display.endWrite();
  }

  return true;
}

// ---- Effects: drawn on the device at the display's full 240x320 ------------

const unsigned long EFFECT_TICK_MS = 40;
unsigned long lastEffectTick = 0;

// Digital rain: a grid of 6x8 character cells. Each column has a bright head
// falling down it, a trail behind, and the end of the trail is erased.
#define RAIN_CELL_W 6
#define RAIN_CELL_H 8
#define RAIN_COLS (SCREEN_WIDTH / RAIN_CELL_W)
#define RAIN_ROWS (SCREEN_HEIGHT / RAIN_CELL_H)
int16_t rainHead[RAIN_COLS];
uint8_t rainLen[RAIN_COLS];
char rainChar[RAIN_COLS];
GFXcanvas16 glyph(RAIN_CELL_W, RAIN_CELL_H);

// Draw one character cell with a single address window (much faster than
// per-pixel drawChar on the display).
void blitGlyph(int col, int row, char c, uint16_t color) {
  glyph.fillScreen(0);
  glyph.drawChar(0, 0, c, color, 0, 1);

  display.startWrite();
  display.setAddrWindow(col * RAIN_CELL_W, row * RAIN_CELL_H, RAIN_CELL_W, RAIN_CELL_H);
  display.writePixels(glyph.getBuffer(), RAIN_CELL_W * RAIN_CELL_H, true, false);
  display.endWrite();
}

char randomGlyph() {
  return (char)(33 + (esp_random() % 94));
}

void startRain() {
  display.fillScreen(ST77XX_BLACK);

  for (int c = 0; c < RAIN_COLS; c++) {
    rainHead[c] = -(int)(esp_random() % RAIN_ROWS);
    rainLen[c] = 6 + esp_random() % 16;
    rainChar[c] = randomGlyph();
  }
}

void tickRain() {
  uint16_t head = display.color565(210, 255, 210);
  uint16_t neck = display.color565(0, 220, 60);
  uint16_t mid = display.color565(0, 120, 30);

  for (int c = 0; c < RAIN_COLS; c++) {
    int h = rainHead[c];

    if (h - 1 >= 0 && h - 1 < RAIN_ROWS) {
      blitGlyph(c, h - 1, rainChar[c], neck);
    }

    if (h >= 0 && h < RAIN_ROWS) {
      rainChar[c] = randomGlyph();
      blitGlyph(c, h, rainChar[c], head);
    }

    int middle = h - rainLen[c] / 2;
    if (middle >= 0 && middle < RAIN_ROWS) {
      blitGlyph(c, middle, randomGlyph(), mid);
    }

    int tail = h - rainLen[c];
    if (tail >= 0 && tail < RAIN_ROWS) {
      display.fillRect(c * RAIN_CELL_W, tail * RAIN_CELL_H, RAIN_CELL_W, RAIN_CELL_H, ST77XX_BLACK);
    }

    rainHead[c]++;

    if (rainHead[c] - rainLen[c] >= RAIN_ROWS) {
      rainHead[c] = -(int)(esp_random() % 12);
      rainLen[c] = 6 + esp_random() % 16;
    }
  }
}

// Static: every pixel a random grey, redrawn in strips.
void tickStatic() {
  for (int row = 0; row < SCREEN_HEIGHT; row += STRIP_ROWS) {
    int rows = STRIP_ROWS;
    if (row + rows > SCREEN_HEIGHT) rows = SCREEN_HEIGHT - row;

    size_t pixels = (size_t)SCREEN_WIDTH * rows;

    for (size_t i = 0; i < pixels; i += 4) {
      uint32_t r = esp_random();

      for (int k = 0; k < 4 && i + k < pixels; k++) {
        uint8_t v = (r >> (k * 8)) & 0xFF;
        pixelBuffer[i + k] = ((uint16_t)(v & 0xF8) << 8) |
                             ((uint16_t)(v & 0xFC) << 3) |
                             (v >> 3);
      }
    }

    display.startWrite();
    display.setAddrWindow(0, row, SCREEN_WIDTH, rows);
    display.writePixels(pixelBuffer, (uint32_t)pixels, true, false);
    display.endWrite();
  }
}

void tickEffect() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  if (shownEffect == TL_EFFECT_RAIN) tickRain();
  else if (shownEffect == TL_EFFECT_STATIC) tickStatic();
}

// ---- Playback --------------------------------------------------------------

// Put the target on screen. Content that is already showing is left alone.
void drawTarget(unsigned long now) {
  if (target.kind == TL_IDLE) {
    if (screenMode != SCREEN_TEXT) {
      enterTextMode();
    }
    return;
  }

  if (target.kind == TL_EFFECT) {
    if (screenMode != SCREEN_EFFECT || shownEffect != target.effect) {
      screenMode = SCREEN_EFFECT;
      shownClip = -1;
      shownFrame = -1;
      shownEffect = target.effect;
      lastEffectTick = 0;

      if (shownEffect == TL_EFFECT_RAIN) {
        startRain();
      } else {
        display.fillScreen(ST77XX_BLACK);
      }
    }
    return;
  }

  // A GIF frame. The same frame is already up when a frame's time spans
  // several changes, e.g. a one-frame GIF.
  if (screenMode == SCREEN_GIF && shownFrame == target.frame) {
    return;
  }

  const TlClip &clip = timeline.clips[target.clip];

  if (screenMode != SCREEN_GIF || shownClip != target.clip) {
    display.fillScreen(ST77XX_BLACK);
  }

  unsigned long drawStart = millis();
  bool drawn = readyFrame && drawFrame(readyFrame, clip);
  unsigned long drawMs = millis() - drawStart;

  if (drawn) {
    screenMode = SCREEN_GIF;
    shownClip = target.clip;
    shownFrame = target.frame;
    shownEffect = TL_EFFECT_NONE;
  }

  // frame=index  src=cache or network  fetch=download time  late=how far past
  // its deadline it was drawn  draw=SPI time.
  Serial.printf("frame=%d src=%s cached=%d/%d fetch=%lu late=%ld draw=%lu heap=%u maxblk=%u\n",
                target.frame,
                readyFromCache ? "cache" : "net",
                cachedFrames,
                (int)timeline.frameCount,
                lastFetchMs,
                (long)(now - changeAt),
                drawMs,
                (unsigned int)ESP.getFreeHeap(),
                (unsigned int)ESP.getMaxAllocHeap());
}

// Get the target's frame ready: from the cache, or the network.
void prepareTarget() {
  if (target.kind != TL_FRAME) {
    targetReady = true;
    return;
  }

  if (screenMode == SCREEN_GIF && shownFrame == target.frame) {
    targetReady = true;  // already on screen
    return;
  }

  unsigned long fetchStart = millis();
  const TlClip &clip = timeline.clips[target.clip];
  size_t pixelCount = (size_t)clip.width * (size_t)clip.height;

  if (target.frame < MAX_CACHE_FRAMES && frameCache[target.frame]) {
    // Already downloaded: no network needed.
    readyFrame = frameCache[target.frame];
    readyFromCache = true;
    targetReady = true;
  } else {
    uint8_t *slot = allocCacheSlot(target.frame, pixelCount);
    uint8_t *dest = slot ? slot : getStreamBuffer(pixelCount);

    targetReady = dest && fetchFrame(target.frame, dest, pixelCount);

    if (targetReady) {
      readyFrame = dest;
      readyFromCache = false;
    } else if (slot) {
      releaseCacheSlot(target.frame, pixelCount);
    }
  }

  lastFetchMs = millis() - fetchStart;
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
    prepareTarget();
    now = millis();
  }

  if (targetReady && (long)(now - changeAt) >= 0) {
    drawTarget(now);

    // The target is drawn from the clock rather than from the previous
    // target, so a slow fetch skips ahead instead of falling behind.
    TlPos current = tlLocate(timeline, tlPosition(timeline, clock_, now));
    changeAt = now + current.untilChangeMs;
    target = tlLocate(timeline, tlPosition(timeline, clock_, changeAt));
    targetReady = false;
    lastFrameFetch = 0;
  }

  if (screenMode == SCREEN_EFFECT) {
    tickEffect();
  }
}

// ---- Server polling and setup ------------------------------------------------

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverOnline = false;
    drawTextScreen();
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP +
               ":5050/api/message?device=" + DEVICE_NAME +
               "&screen=st7735";

  http.setTimeout(5000);

  if (!http.begin(client, url)) {
    serverOnline = false;
    drawTextScreen();
    return;
  }

  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String response = http.getString();
    JSONVar doc = JSON.parse(response);

    if (JSON.typeof(doc) != "undefined") {
      currentText = (const char *)doc["text"];
      serverOnline = true;
    } else {
      Serial.println("Message poll: invalid JSON");
      serverOnline = false;
    }
  } else {
    Serial.printf("Message poll: HTTP %d\n", code);
    serverOnline = false;
  }

  http.end();

  // drawTextScreen() clears the display before showing the current message.
  // It does nothing while a GIF or effect is on screen.
  drawTextScreen();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  tlClear(timeline);
  clock_.synced = false;
  clock_.startMs = 0;

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  display.init(SCREEN_WIDTH, SCREEN_HEIGHT);
  display.setRotation(0);
  showStartup();

  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  Serial.printf("Largest free block: %u\n", ESP.getMaxAllocHeap());

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

  refreshAnimation();
  drawTextScreen();

  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    pollServer();
    refreshAnimation();
  }

  if (tlActive(timeline)) {
    playTimeline();
  }

  delay(2);
}
