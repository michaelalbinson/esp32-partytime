#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <math.h>
#include <new>
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

// Offscreen buffer for effects and text that are composed in memory and sent
// in one write (no flicker). Allocated when such a mode starts and freed when
// it ends, so its RAM goes back to the frame cache.
GFXcanvas16 *canvas = nullptr;

void freeCanvas() {
  if (canvas) {
    delete canvas;
    canvas = nullptr;
  }
}

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
enum ScreenMode { SCREEN_TEXT, SCREEN_GIF, SCREEN_EFFECT, SCREEN_TEXTSTEP };
ScreenMode screenMode = SCREEN_TEXT;
int shownClip = -1;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;
int shownStep = -1;   // the timeline step an effect or text style belongs to
int shownStyle = TL_STYLE_NONE;
int shownText = 0;

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

// The text screen is only redrawn when it changed or something else drew over
// it; redrawing every poll made the whole screen flash.
bool textDirty = true;
String drawnText = "";
bool drawnOnline = false;

void drawTextScreen() {
  if (screenMode != SCREEN_TEXT) {
    return;
  }

  if (!textDirty && drawnText == currentText && drawnOnline == serverOnline) {
    return;
  }

  textDirty = false;
  drawnText = currentText;
  drawnOnline = serverOnline;

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
  freeCanvas();
  screenMode = SCREEN_TEXT;
  shownClip = -1;
  shownFrame = -1;
  shownEffect = TL_EFFECT_NONE;
  shownStep = -1;
  shownStyle = TL_STYLE_NONE;
  textDirty = true;  // whatever was on screen before is still there
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

    if (tlActive(timeline)) {
      // Leave the old picture up while the new program loads (see
      // preloadFrames); the new program's first draw replaces it. Forget what
      // was shown so that draw can't be skipped as "already on screen".
      freeCanvas();
      screenMode = SCREEN_TEXT;
      shownClip = -1;
      shownFrame = -1;
      shownEffect = TL_EFFECT_NONE;
      shownStep = -1;
      shownStyle = TL_STYLE_NONE;
      textDirty = true;
    } else {
      // Nothing to play: clear the old content and show the message.
      enterTextMode();
    }

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
// Loading progress: a single pixel row along the top edge that grows left to
// right. It is drawn over whatever is on screen (nothing starts that high) so
// it doesn't interrupt anything.
#define LOADING_BAR_HEIGHT 1

void drawLoadingBar(int done, int total) {
  if (done > 0) {
    display.fillRect(0, 0, (SCREEN_WIDTH * done) / total, LOADING_BAR_HEIGHT, ST77XX_GREEN);
  }
}

void preloadFrames() {
  drawLoadingBar(0, timeline.frameCount);

  for (int f = 0; f < timeline.frameCount; f++) {
    int clipIndex = clipOfFrame(f);
    if (clipIndex < 0) continue;

    const TlClip &c = timeline.clips[clipIndex];
    size_t pixels = (size_t)c.width * (size_t)c.height;
    uint8_t *slot = allocCacheSlot(f, pixels);

    if (!slot) {
      break;  // cache full; the rest streams
    }

    if (!fetchFrame(f, slot, pixels)) {
      releaseCacheSlot(f, pixels);
      break;
    }

    drawLoadingBar(f + 1, timeline.frameCount);
  }

  display.fillRect(0, 0, SCREEN_WIDTH, LOADING_BAR_HEIGHT, ST77XX_BLACK);

  Serial.printf("Preloaded %d of %d frames (%u bytes)\n",
                cachedFrames, (int)timeline.frameCount, (unsigned int)cacheBytes);
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

// RGB332 -> RGB565, built once. Red/green/blue are widened by repeating their
// top bits so full intensity stays full.
uint16_t rgb332Lut[256];

void initColorLut() {
  for (int c = 0; c < 256; c++) {
    uint8_t r3 = (c >> 5) & 0x07;
    uint8_t g3 = (c >> 2) & 0x07;
    uint8_t b2 = c & 0x03;

    uint8_t r5 = (r3 << 2) | (r3 >> 1);
    uint8_t g6 = (g3 << 3) | g3;
    uint8_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);

    rgb332Lut[c] = ((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | b5;
  }
}

// The server shrinks frames so a whole show fits the RAM cache, so they are
// often small. Draw each at the largest whole-number scale that fits the
// screen (nearest neighbour: no extra RAM, just more pixels over SPI).
#define MAX_FRAME_SCALE 8

int clipScale(const TlClip &clip) {
  int scale = min(SCREEN_WIDTH / (int)clip.width, SCREEN_HEIGHT / (int)clip.height);
  if (scale < 1) scale = 1;
  if (scale > MAX_FRAME_SCALE) scale = MAX_FRAME_SCALE;
  return scale;
}

// Convert a frame, scale it up and draw it centered, in RGB565 strips.
bool drawFrame(const uint8_t *src, const TlClip &clip) {
  int scale = clipScale(clip);
  int scaledWidth = (int)clip.width * scale;
  int scaledHeight = (int)clip.height * scale;
  int originX = (SCREEN_WIDTH - scaledWidth) / 2;
  int originY = (SCREEN_HEIGHT - scaledHeight) / 2;

  for (int row = 0; row < scaledHeight; row += STRIP_ROWS) {
    int rows = STRIP_ROWS;

    if (row + rows > scaledHeight) {
      rows = scaledHeight - row;
    }

    size_t stripPixels = (size_t)scaledWidth * (size_t)rows;

    if (stripPixels > MAX_STRIP_PIXELS) {
      Serial.println("Strip exceeds RGB565 buffer");
      return false;
    }

    int lastSourceRow = -1;

    for (int r = 0; r < rows; r++) {
      int sourceRow = (row + r) / scale;
      uint16_t *out = pixelBuffer + (size_t)r * scaledWidth;

      if (sourceRow == lastSourceRow) {
        // A scaled-up row repeats the one above it.
        memcpy(out, out - scaledWidth, (size_t)scaledWidth * sizeof(uint16_t));
        continue;
      }

      lastSourceRow = sourceRow;
      const uint8_t *line = src + (size_t)sourceRow * clip.width;

      for (int x = 0; x < clip.width; x++) {
        uint16_t color = rgb332Lut[line[x]];
        for (int k = 0; k < scale; k++) {
          *out++ = color;
        }
      }
    }

    display.startWrite();
    display.setAddrWindow(originX, originY + row, scaledWidth, rows);
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

// ---- Shared helpers for the effects and text styles ------------------------

// Milliseconds the current effect/text step has been running. Everything that
// moves is a function of this, so it moves identically on every display.
uint32_t stepElapsed() {
  return tlStepElapsed(timeline, clock_, millis(), shownStep);
}

uint16_t hsv565(int hue, uint8_t sat, uint8_t val) {
  hue = ((hue % 360) + 360) % 360;
  int region = hue / 60;
  int rem = (hue % 60) * 255 / 60;
  int p = val * (255 - sat) / 255;
  int q = val * (255 - sat * rem / 255) / 255;
  int t = val * (255 - sat * (255 - rem) / 255) / 255;
  int r, g, b;

  switch (region) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
  }

  return display.color565(r, g, b);
}

// Position along 0..span for a point that has travelled "distance" and bounces
// off both ends.
float triangleWave(float distance, float span) {
  if (span <= 0) return 0;
  float d = fmodf(distance, 2 * span);
  return d < span ? d : 2 * span - d;
}

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Allocate the shared offscreen canvas (replacing any existing one). Returns
// false if there isn't the memory; callers then skip drawing.
bool allocCanvas(int w, int h) {
  freeCanvas();
  canvas = new (std::nothrow) GFXcanvas16(w, h);
  if (canvas && !canvas->getBuffer()) {
    freeCanvas();
  }
  return canvas != nullptr;
}

// Draw the whole canvas at x,y with a single address window.
void blitCanvas(int x, int y) {
  if (!canvas) return;

  display.startWrite();
  display.setAddrWindow(x, y, canvas->width(), canvas->height());
  display.writePixels(canvas->getBuffer(), (uint32_t)canvas->width() * canvas->height(), true, false);
  display.endWrite();
}

// Black out the part of the old rectangle that the new one doesn't cover.
void eraseOldRect(int ox, int oy, int nx, int ny, int w, int h) {
  int ox2 = ox + w, oy2 = oy + h, nx2 = nx + w, ny2 = ny + h;

  if (nx >= ox2 || nx2 <= ox || ny >= oy2 || ny2 <= oy) {
    display.fillRect(ox, oy, w, h, ST77XX_BLACK);
    return;
  }

  if (oy < ny) display.fillRect(ox, oy, w, ny - oy, ST77XX_BLACK);
  if (oy2 > ny2) display.fillRect(ox, ny2, w, oy2 - ny2, ST77XX_BLACK);

  int top = oy > ny ? oy : ny;
  int bottom = oy2 < ny2 ? oy2 : ny2;
  if (ox < nx) display.fillRect(ox, top, nx - ox, bottom - top, ST77XX_BLACK);
  if (ox2 > nx2) display.fillRect(nx2, top, ox2 - nx2, bottom - top, ST77XX_BLACK);
}

// ---- Equalizer: 16 bars of 32 segments, levels from layered sines ------------

#define EQ_BARS 16
#define EQ_BAR_W 14      // 16 bars + 15 gaps of 1 px = 239 px
#define EQ_PITCH 15
#define EQ_SEGMENTS 32
#define EQ_SEG_H 10
uint8_t eqLevel[EQ_BARS];

uint16_t eqColor(int seg) {
  float f = seg / (float)(EQ_SEGMENTS - 1);
  int r, g;
  if (f < 0.5f) {
    r = (int)(f * 2 * 255);
    g = 220;
  } else {
    r = 255;
    g = (int)(220 * (1 - (f - 0.5f) * 2));
  }
  return display.color565(r, g, 0);
}

void startEqualizer() {
  memset(eqLevel, 0, sizeof(eqLevel));
}

void tickEqualizer() {
  float t = (float)stepElapsed();

  for (int i = 0; i < EQ_BARS; i++) {
    float l = (sinf(t * 0.004f * (1 + i % 3) + i * 1.7f) +
               sinf(t * 0.0027f * (1 + i % 4) + i * 0.9f) + 2.0f) / 4.0f;
    int level = (int)(l * EQ_SEGMENTS + 0.5f);
    if (level < 0) level = 0;
    if (level > EQ_SEGMENTS) level = EQ_SEGMENTS;

    int x = i * EQ_PITCH;

    for (int k = eqLevel[i]; k < level; k++) {
      display.fillRect(x, SCREEN_HEIGHT - (k + 1) * EQ_SEG_H, EQ_BAR_W, EQ_SEG_H - 1, eqColor(k));
    }
    for (int k = level; k < eqLevel[i]; k++) {
      display.fillRect(x, SCREEN_HEIGHT - (k + 1) * EQ_SEG_H, EQ_BAR_W, EQ_SEG_H, ST77XX_BLACK);
    }

    eqLevel[i] = level;
  }
}

// ---- Scanner: a red eye sweeping across the middle ---------------------------

#define SCANNER_BAND_ROWS 16
#define SCANNER_PERIOD_MS 1600
#define SCANNER_HEAD_W 12
#define SCANNER_TRAIL 80

void tickScanner() {
  float phase = (float)(stepElapsed() % SCANNER_PERIOD_MS) / (SCANNER_PERIOD_MS / 2);
  float p = phase < 1.0f ? phase : 2.0f - phase;
  float dir = phase < 1.0f ? 1.0f : -1.0f;
  float headX = SCANNER_HEAD_W / 2 + p * (SCREEN_WIDTH - SCANNER_HEAD_W);

  // Brightness of each column, ahead of the head (little), on it, and behind.
  for (int x = 0; x < SCREEN_WIDTH; x++) {
    float d = (x - headX) * dir;
    float f = 0;

    if (d >= -SCANNER_HEAD_W / 2 && d <= SCANNER_HEAD_W / 2) f = 1.0f;
    else if (d > SCANNER_HEAD_W / 2 && d < SCANNER_HEAD_W / 2 + 4) f = 0.3f;
    else if (d < -SCANNER_HEAD_W / 2 && d > -SCANNER_HEAD_W / 2 - SCANNER_TRAIL) {
      f = 1.0f - (-d - SCANNER_HEAD_W / 2) / SCANNER_TRAIL;
      f *= f;
    }

    pixelBuffer[x] = display.color565((int)(255 * f), (int)(70 * f * f * f), 0);
  }

  for (int row = 1; row < SCANNER_BAND_ROWS; row++) {
    memcpy(&pixelBuffer[row * SCREEN_WIDTH], pixelBuffer, SCREEN_WIDTH * sizeof(uint16_t));
  }

  display.startWrite();
  display.setAddrWindow(0, (SCREEN_HEIGHT - SCANNER_BAND_ROWS) / 2, SCREEN_WIDTH, SCANNER_BAND_ROWS);
  display.writePixels(pixelBuffer, SCREEN_WIDTH * SCANNER_BAND_ROWS, true, false);
  display.endWrite();
}

// ---- Radar: a sweeping beam with a fading wedge trail and blips --------------

#define RADAR_CX 120
#define RADAR_CY 160
#define RADAR_R 108
#define RADAR_PERIOD_MS 3000
#define RADAR_WEDGES 8
#define RADAR_WEDGE_ANGLE 0.07f
#define RADAR_BLIPS 6
#define RADAR_BLIP_FADE_MS 1500
float radarPrevAngle = 0;
bool radarHavePrev = false;
int16_t blipX[RADAR_BLIPS], blipY[RADAR_BLIPS];
float blipAngle[RADAR_BLIPS];

void radarWedge(float a0, float a1, uint16_t color) {
  display.fillTriangle(RADAR_CX, RADAR_CY,
                       RADAR_CX + (int)(RADAR_R * cosf(a0)), RADAR_CY + (int)(RADAR_R * sinf(a0)),
                       RADAR_CX + (int)(RADAR_R * cosf(a1)), RADAR_CY + (int)(RADAR_R * sinf(a1)),
                       color);
}

void startRadar() {
  radarHavePrev = false;
  display.drawCircle(RADAR_CX, RADAR_CY, RADAR_R + 4, display.color565(0, 70, 20));

  for (int i = 0; i < RADAR_BLIPS; i++) {
    float angle = (hash32(i * 2 + 1) % 6283) / 1000.0f;
    float radius = 20 + (hash32(i * 2 + 2) % 85);
    blipAngle[i] = angle;
    blipX[i] = RADAR_CX + (int)(radius * cosf(angle));
    blipY[i] = RADAR_CY + (int)(radius * sinf(angle));
  }
}

void tickRadar() {
  float theta = 6.2831853f * (float)stepElapsed() / RADAR_PERIOD_MS;
  const float span = RADAR_WEDGES * RADAR_WEDGE_ANGLE;

  // Erase the sliver of trail that has fallen off the tail since last tick, in
  // pieces small enough that the chord of each stays inside the arc.
  if (radarHavePrev) {
    float delta = theta - radarPrevAngle;

    if (delta < 0 || delta > 1.0f) {
      display.fillCircle(RADAR_CX, RADAR_CY, RADAR_R + 1, ST77XX_BLACK);
    } else {
      float a = radarPrevAngle - span - 0.02f;
      float end = theta - span;
      while (a < end) {
        float next = a + 0.1f < end ? a + 0.1f : end;
        radarWedge(a, next, ST77XX_BLACK);
        a = next;
      }
    }
  }

  for (int j = 0; j < RADAR_WEDGES; j++) {
    float f = 1.0f - (float)j / RADAR_WEDGES;
    int g = (int)(255 * f * f);
    radarWedge(theta - (j + 1) * RADAR_WEDGE_ANGLE, theta - j * RADAR_WEDGE_ANGLE,
               display.color565(0, g, g / 6));
  }

  display.drawLine(RADAR_CX, RADAR_CY,
                   RADAR_CX + (int)(RADAR_R * cosf(theta)), RADAR_CY + (int)(RADAR_R * sinf(theta)),
                   display.color565(190, 255, 190));

  // Blips glow when the beam passes over them, then fade.
  float beam = fmodf(theta, 6.2831853f);
  for (int i = 0; i < RADAR_BLIPS; i++) {
    float behind = fmodf(beam - blipAngle[i] + 12.566371f, 6.2831853f);
    float ageMs = behind / 6.2831853f * RADAR_PERIOD_MS;
    float b = ageMs < RADAR_BLIP_FADE_MS ? 1.0f - ageMs / RADAR_BLIP_FADE_MS : 0;
    display.fillRect(blipX[i] - 1, blipY[i] - 1, 3, 3,
                     display.color565((int)(160 * b), (int)(255 * b), (int)(160 * b)));
  }

  // The wedges and erasing nick the crosshair, so redraw it each tick.
  uint16_t cross = display.color565(0, 60, 20);
  display.drawFastHLine(RADAR_CX - RADAR_R, RADAR_CY, 2 * RADAR_R + 1, cross);
  display.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_R, 2 * RADAR_R + 1, cross);

  radarPrevAngle = theta;
  radarHavePrev = true;
}

// ---- Test pattern: SMPTE-style color bars, drawn once ------------------------

void startTestPattern() {
  const uint16_t top[7] = {
    display.color565(192, 192, 192), display.color565(192, 192, 0),
    display.color565(0, 192, 192),   display.color565(0, 192, 0),
    display.color565(192, 0, 192),   display.color565(192, 0, 0),
    display.color565(0, 0, 192),
  };
  const uint16_t reverse[7] = {
    display.color565(0, 0, 192),     ST77XX_BLACK,
    display.color565(192, 0, 192),   ST77XX_BLACK,
    display.color565(0, 192, 192),   ST77XX_BLACK,
    display.color565(192, 192, 192),
  };

  const int topH = SCREEN_HEIGHT * 7 / 10;
  const int midH = SCREEN_HEIGHT / 12;

  for (int i = 0; i < 7; i++) {
    int x0 = i * SCREEN_WIDTH / 7;
    int x1 = (i + 1) * SCREEN_WIDTH / 7;
    display.fillRect(x0, 0, x1 - x0, topH, top[i]);
    display.fillRect(x0, topH, x1 - x0, midH, reverse[i]);
  }

  // PLUGE-style strip: -I, white, +Q, black, then the sub/super-black steps.
  const int y = topH + midH;
  const int h = SCREEN_HEIGHT - y;
  const int w = SCREEN_WIDTH / 6;
  display.fillRect(0, y, w, h, display.color565(0, 33, 76));
  display.fillRect(w, y, w, h, display.color565(255, 255, 255));
  display.fillRect(2 * w, y, w, h, display.color565(50, 0, 106));
  display.fillRect(3 * w, y, w, h, ST77XX_BLACK);
  display.fillRect(4 * w, y, w / 3, h, display.color565(0, 0, 0));
  display.fillRect(4 * w + w / 3, y, w / 3, h, display.color565(10, 10, 10));
  display.fillRect(4 * w + 2 * w / 3, y, w / 3, h, display.color565(20, 20, 20));
  display.fillRect(5 * w, y, SCREEN_WIDTH - 5 * w, h, display.color565(30, 30, 30));

  // The device's name in a black box in the middle.
  int len = strlen(DEVICE_NAME);
  int size = (len * 12 + 16 <= 200) ? 2 : 1;
  int boxW = len * 6 * size + 16;
  int boxH = 8 * size + 14;
  if (boxW > SCREEN_WIDTH) boxW = SCREEN_WIDTH;
  int bx = (SCREEN_WIDTH - boxW) / 2;
  int by = (SCREEN_HEIGHT - boxH) / 2;

  display.fillRect(bx, by, boxW, boxH, ST77XX_BLACK);
  display.drawRect(bx, by, boxW, boxH, ST77XX_WHITE);
  display.setTextSize(size);
  display.setTextWrap(false);
  display.setTextColor(ST77XX_WHITE);
  display.setCursor(bx + 8, by + 7);
  display.print(DEVICE_NAME);
  display.setTextWrap(true);
}

// ---- Snow and confetti: particles falling at different speeds -----------------

#define MAX_FLAKES 50
struct Flake {
  int16_t baseX, y;
  int16_t drawnX, drawnY;  // where it was last drawn; drawnX < -500 = not yet
  uint8_t speed, size, phase;
  uint16_t color;
};
Flake flakes[MAX_FLAKES];
int flakeCount = 0;
bool flakesAreConfetti = false;

void resetFlake(int index, bool anywhere) {
  Flake &f = flakes[index];
  f.baseX = esp_random() % SCREEN_WIDTH;
  f.size = flakesAreConfetti ? 3 + esp_random() % 3 : 1 + esp_random() % 3;
  f.speed = flakesAreConfetti ? 2 + esp_random() % 5 : 1 + esp_random() % 4;
  f.phase = esp_random() % 64;
  f.y = anywhere ? (int)(esp_random() % SCREEN_HEIGHT) : -(int)f.size;

  if (flakesAreConfetti) {
    f.color = hsv565(esp_random() % 360, 255, 255);
  } else {
    int grey = 190 + esp_random() % 66;
    f.color = display.color565(grey, grey, grey);
  }
}

void startFlakes(bool confetti) {
  flakesAreConfetti = confetti;
  flakeCount = confetti ? 40 : 50;

  for (int i = 0; i < flakeCount; i++) {
    resetFlake(i, true);
    flakes[i].drawnX = -1000;
  }
}

void tickFlakes() {
  float sway = flakesAreConfetti ? 10.0f : 4.0f;

  for (int i = 0; i < flakeCount; i++) {
    Flake &f = flakes[i];

    if (f.drawnX > -500) {
      display.fillRect(f.drawnX, f.drawnY, f.size, f.size, ST77XX_BLACK);
    }

    f.y += f.speed;
    if (f.y >= SCREEN_HEIGHT) {
      resetFlake(i, false);
    }

    int x = f.baseX + (int)(sway * sinf(f.y * 0.04f + f.phase * 0.1f));
    if (x < 0) x = 0;
    if (x > SCREEN_WIDTH - f.size) x = SCREEN_WIDTH - f.size;

    display.fillRect(x, f.y, f.size, f.size, f.color);
    f.drawnX = x;
    f.drawnY = f.y;
  }
}

// ---- Bounce: the device name in a box, bouncing off the edges -----------------

#define BOUNCE_PAD 6
#define BOUNCE_VX 70.0f   // px per second
#define BOUNCE_VY 50.0f
int bounceW = 0, bounceH = 0, bounceSize = 3;
int bounceX = 0, bounceY = 0;
long bounceHits = -1;
bool bounceDrawn = false;

void startBounce() {
  int len = strlen(DEVICE_NAME);
  bounceSize = 3;
  if (len * 6 * bounceSize + 2 * BOUNCE_PAD > SCREEN_WIDTH) bounceSize = 2;
  if (len * 6 * bounceSize + 2 * BOUNCE_PAD > SCREEN_WIDTH) bounceSize = 1;

  bounceW = len * 6 * bounceSize + 2 * BOUNCE_PAD;
  if (bounceW > SCREEN_WIDTH) bounceW = SCREEN_WIDTH;
  bounceH = 8 * bounceSize + 2 * BOUNCE_PAD;

  bounceHits = -1;
  bounceDrawn = false;
  allocCanvas(bounceW, bounceH);
}

void tickBounce() {
  if (!canvas) return;

  float t = (float)stepElapsed() / 1000.0f;
  float spanX = SCREEN_WIDTH - bounceW;
  float spanY = SCREEN_HEIGHT - bounceH;
  float distX = t * BOUNCE_VX;
  float distY = t * BOUNCE_VY;

  int x = (int)triangleWave(distX, spanX);
  int y = (int)triangleWave(distY, spanY);
  long hits = (spanX > 0 ? (long)(distX / spanX) : 0) + (spanY > 0 ? (long)(distY / spanY) : 0);

  // The box only changes when the color does.
  if (hits != bounceHits) {
    bounceHits = hits;
    uint16_t color = hsv565((int)(hits * 67 % 360), 255, 255);

    canvas->fillScreen(0);
    canvas->drawRect(0, 0, bounceW, bounceH, color);
    canvas->setTextWrap(false);
    canvas->setTextSize(bounceSize);
    canvas->setTextColor(color);
    canvas->setCursor(BOUNCE_PAD, BOUNCE_PAD);
    canvas->print(DEVICE_NAME);
  }

  if (bounceDrawn) {
    eraseOldRect(bounceX, bounceY, x, y, bounceW, bounceH);
  }

  blitCanvas(x, y);
  bounceX = x;
  bounceY = y;
  bounceDrawn = true;
}

// ---- Effect dispatch -------------------------------------------------------

// Set up an effect on a freshly cleared screen.
void startEffect(int effect) {
  switch (effect) {
    case TL_EFFECT_RAIN: startRain(); break;
    case TL_EFFECT_EQUALIZER: startEqualizer(); break;
    case TL_EFFECT_RADAR: startRadar(); break;
    case TL_EFFECT_TESTPATTERN: startTestPattern(); break;
    case TL_EFFECT_SNOW: startFlakes(false); break;
    case TL_EFFECT_CONFETTI: startFlakes(true); break;
    case TL_EFFECT_BOUNCE: startBounce(); break;
    default: break;  // static and scanner need no setup
  }
}

void tickEffect() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  switch (shownEffect) {
    case TL_EFFECT_RAIN: tickRain(); break;
    case TL_EFFECT_STATIC: tickStatic(); break;
    case TL_EFFECT_EQUALIZER: tickEqualizer(); break;
    case TL_EFFECT_SCANNER: tickScanner(); break;
    case TL_EFFECT_RADAR: tickRadar(); break;
    case TL_EFFECT_SNOW:
    case TL_EFFECT_CONFETTI: tickFlakes(); break;
    case TL_EFFECT_BOUNCE: tickBounce(); break;
    default: break;  // the test pattern is drawn once
  }
}

// ---- Text styles: the text from the show, drawn big -------------------------

#define TEXT_STRIP_H 32       // one line of size-4 (marquee) or size-2 + bob (wave)
#define MARQUEE_SIZE 4
#define MARQUEE_PX_PER_S 100
#define MARQUEE_GAP_CHARS 3
#define FLASH_WORD_MS 450
#define WAVE_COLS (SCREEN_WIDTH / 12)
#define WAVE_MAX_LINES (SCREEN_HEIGHT / TEXT_STRIP_H)

int shownWord = -1;
int waveLineStart[WAVE_MAX_LINES];
int waveLineLen[WAVE_MAX_LINES];
int waveLines = 0;

int wordCount(const char *text) {
  int count = 0;
  bool inWord = false;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') inWord = false;
    else if (!inWord) { inWord = true; count++; }
  }
  return count;
}

// Copy the n-th space-separated word into out. Returns its length.
int nthWord(const char *text, int n, char *out, int outSize) {
  int count = -1;
  bool inWord = false;
  int len = 0;

  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      if (inWord && count == n) break;
      inWord = false;
    } else {
      if (!inWord) { inWord = true; count++; }
      if (count == n && len < outSize - 1) out[len++] = *p;
    }
  }

  out[len] = '\0';
  return len;
}

// Break the text into lines of at most WAVE_COLS chars, at spaces if possible.
void layoutWave(const char *text) {
  int len = strlen(text);
  int i = 0;
  waveLines = 0;

  while (i < len && waveLines < WAVE_MAX_LINES) {
    while (i < len && text[i] == ' ') i++;
    if (i >= len) break;

    int lineLen;
    int next;

    if (len - i <= WAVE_COLS) {
      lineLen = len - i;
      next = len;
    } else {
      int j = i + WAVE_COLS;
      while (j > i && text[j] != ' ') j--;

      if (j > i) {
        lineLen = j - i;
        next = j + 1;
      } else {
        lineLen = WAVE_COLS;
        next = i + WAVE_COLS;
      }
    }

    waveLineStart[waveLines] = i;
    waveLineLen[waveLines] = lineLen;
    waveLines++;
    i = next;
  }
}

void startTextStyle(int style, const char *text) {
  shownWord = -1;

  if (style == TL_STYLE_MARQUEE || style == TL_STYLE_WAVE) {
    allocCanvas(SCREEN_WIDTH, TEXT_STRIP_H);
  }
  if (style == TL_STYLE_WAVE) {
    layoutWave(text);
  }
}

void tickMarquee(const char *text) {
  if (!canvas || !text[0]) return;

  const int charW = 6 * MARQUEE_SIZE;
  int len = strlen(text);
  int period = (len + MARQUEE_GAP_CHARS) * charW;
  uint32_t t = stepElapsed();
  int offset = (int)(((uint64_t)t * MARQUEE_PX_PER_S / 1000) % period);
  uint16_t color = hsv565((int)(t / 25), 160, 255);

  canvas->fillScreen(0);
  canvas->setTextWrap(false);

  // The line (text plus a gap) repeats, so the strip is always full.
  for (int base = -offset; base < SCREEN_WIDTH; base += period) {
    for (int i = 0; i < len; i++) {
      int x = base + i * charW;
      if (x + charW <= 0 || x >= SCREEN_WIDTH) continue;
      canvas->drawChar(x, 0, text[i], color, 0, MARQUEE_SIZE);
    }
  }

  blitCanvas(0, (SCREEN_HEIGHT - TEXT_STRIP_H) / 2);
}

void tickFlash(const char *text) {
  int words = wordCount(text);
  if (words == 0) return;

  int index = (int)((stepElapsed() / FLASH_WORD_MS) % words);
  if (index == shownWord) return;
  shownWord = index;

  char word[TL_TEXT_LEN];
  int len = nthWord(text, index, word, sizeof(word));

  int size = SCREEN_WIDTH / (6 * len);
  if (size > 10) size = 10;
  if (size < 1) size = 1;

  int width = len * 6 * size;
  int x = (SCREEN_WIDTH - width) / 2;
  if (x < 0) x = 0;

  display.fillScreen(ST77XX_BLACK);
  display.setTextWrap(false);
  display.setTextSize(size);
  display.setTextColor(hsv565(index * 57, 255, 255));
  display.setCursor(x, (SCREEN_HEIGHT - 8 * size) / 2);
  display.print(word);
  display.setTextWrap(true);
}

void tickWave(const char *text) {
  if (!canvas || waveLines == 0) return;

  float t = (float)stepElapsed();
  int top = (SCREEN_HEIGHT - waveLines * TEXT_STRIP_H) / 2;

  for (int line = 0; line < waveLines; line++) {
    int len = waveLineLen[line];
    int x0 = (SCREEN_WIDTH - len * 12) / 2;

    canvas->fillScreen(0);

    for (int c = 0; c < len; c++) {
      int dy = (int)roundf(6.0f * sinf(t * 0.006f + c * 0.5f + line * 0.7f));
      uint16_t color = hsv565((int)(t / 12.0f) + c * 18 + line * 30, 255, 255);
      canvas->drawChar(x0 + c * 12, 8 + dy, text[waveLineStart[line] + c], color, 0, 2);
    }

    blitCanvas(0, top + line * TEXT_STRIP_H);
  }
}

void tickTextStep() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  const char *text = timeline.texts[shownText];

  if (shownStyle == TL_STYLE_MARQUEE) tickMarquee(text);
  else if (shownStyle == TL_STYLE_FLASH) tickFlash(text);
  else if (shownStyle == TL_STYLE_WAVE) tickWave(text);
}

// ---- Playback --------------------------------------------------------------

// Put the target on screen. Content that is already showing is left alone.
void drawTarget(unsigned long now) {
  if (target.kind == TL_IDLE) {
    if (screenMode != SCREEN_TEXT) {
      enterTextMode();
    } else {
      drawTextScreen();  // redraws only if the screen isn't already showing it
    }
    return;
  }

  if (target.kind == TL_EFFECT) {
    shownStep = target.step;

    if (screenMode != SCREEN_EFFECT || shownEffect != target.effect) {
      freeCanvas();
      screenMode = SCREEN_EFFECT;
      shownClip = -1;
      shownFrame = -1;
      shownEffect = target.effect;
      shownStyle = TL_STYLE_NONE;
      lastEffectTick = 0;

      display.fillScreen(ST77XX_BLACK);
      startEffect(shownEffect);
    }
    return;
  }

  if (target.kind == TL_TEXT) {
    if (screenMode != SCREEN_TEXTSTEP || shownStep != target.step) {
      freeCanvas();
      screenMode = SCREEN_TEXTSTEP;
      shownClip = -1;
      shownFrame = -1;
      shownEffect = TL_EFFECT_NONE;
      shownStep = target.step;
      shownStyle = target.style;
      shownText = target.text;
      lastEffectTick = 0;

      display.fillScreen(ST77XX_BLACK);
      startTextStyle(shownStyle, timeline.texts[shownText]);
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
    freeCanvas();
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
    shownStyle = TL_STYLE_NONE;
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
  } else if (screenMode == SCREEN_TEXTSTEP) {
    tickTextStep();
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
  initColorLut();

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
