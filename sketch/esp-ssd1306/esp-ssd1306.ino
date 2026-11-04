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
enum ScreenMode { SCREEN_TEXT, SCREEN_GIF, SCREEN_EFFECT };
ScreenMode screenMode = SCREEN_TEXT;
int shownFrame = -1;
int shownEffect = TL_EFFECT_NONE;
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

void tickEffect() {
  unsigned long now = millis();
  if (now - lastEffectTick < EFFECT_TICK_MS) return;
  lastEffectTick = now;

  if (shownEffect == TL_EFFECT_RAIN) tickRain();
  else if (shownEffect == TL_EFFECT_STATIC) tickStatic();
}

// ---- Playback ----------------------------------------------------------------

// Put the target on screen. Content that is already showing is left alone.
void drawTarget() {
  if (target.kind == TL_IDLE) {
    if (screenMode != SCREEN_TEXT) enterTextMode();
    return;
  }

  if (target.kind == TL_EFFECT) {
    if (screenMode != SCREEN_EFFECT || shownEffect != target.effect) {
      screenMode = SCREEN_EFFECT;
      shownFrame = -1;
      shownEffect = target.effect;
      lastEffectTick = 0;
      if (shownEffect == TL_EFFECT_RAIN) startRain();
    }
    return;
  }

  if (screenMode == SCREEN_GIF && shownFrame == target.frame) return;

  drawAnimationFrame(target.frame);
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

  if (screenMode == SCREEN_EFFECT) tickEffect();

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
