#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <secrets.h>
#include "timeline.h"

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
uint8_t frameBitmap[SOURCE_BYTES];
unsigned long lastAnimationCheck = 0;

// What is on the matrix. There is no text screen: a wait just clears it.
enum ScreenMode { SCREEN_BLANK, SCREEN_GIF, SCREEN_EFFECT };
ScreenMode screenMode = SCREEN_BLANK;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;
const unsigned long EFFECT_TICK_MS = 80;
unsigned long lastEffectTick = 0;

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

// Ask the server what to play. Returns false if it couldn't be reached.
bool checkAnimation() {
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

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    haveTarget = false;
    targetReady = false;
    clock_.synced = false;

    if (!tlLoadJson(timeline, info)) {
      Serial.println("Bad program, ignoring it");
      tlClear(timeline);
    }

    // Clear whatever was playing.
    screenMode = SCREEN_BLANK;
    shownFrame = -1;
    shownEffect = TL_EFFECT_NONE;
    matrix.clear();
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

void tickEffect() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  if (shownEffect == TL_EFFECT_RAIN) tickRain();
  else if (shownEffect == TL_EFFECT_STATIC) tickStatic();
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
    }
    return;
  }

  if (target.kind == TL_EFFECT) {
    if (screenMode != SCREEN_EFFECT || shownEffect != target.effect) {
      screenMode = SCREEN_EFFECT;
      shownFrame = -1;
      shownEffect = target.effect;
      lastEffectTick = 0;
      matrix.clear();
      if (shownEffect == TL_EFFECT_RAIN) startRain();
    }
    return;
  }

  if (screenMode == SCREEN_GIF && shownFrame == target.frame) return;

  showMatrixFrame(frameBitmap);
  screenMode = SCREEN_GIF;
  shownFrame = target.frame;
  shownEffect = TL_EFFECT_NONE;
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
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(500);
    return;
  }

  unsigned long now = millis();

  if (now - lastAnimationCheck >= ANIMATION_CHECK_MS) {
    lastAnimationCheck = now;
    checkAnimation();
  }

  if (tlActive(timeline)) playTimeline();

  delay(10);
}
