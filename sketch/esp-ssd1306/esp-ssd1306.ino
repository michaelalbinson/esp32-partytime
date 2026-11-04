#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino_JSON.h>
#include <secrets.h>

const int OLED_SDA = 21;
const int OLED_SCL = 22;
const unsigned long POLL_INTERVAL_MS = 2000;

int animationVersion = -1;
int animationFrameCount = 0;
int animationFrameIndex = 0;
unsigned long animationFrameMs = 120;

// Per-frame display time from the GIF. Falls back to animationFrameMs.
#define MAX_ANIMATION_FRAMES 60
uint16_t frameDelays[MAX_ANIMATION_FRAMES];
// The next frame is fetched right after the current one is drawn, then drawn
// at nextFrameAt. Network latency therefore only matters if a fetch takes
// longer than the frame's delay.
bool frameReady = false;         // buffer holds frame animationFrameIndex
bool animationResync = true;     // next draw starts a fresh schedule
unsigned long nextFrameAt = 0;   // millis() deadline to draw the buffered frame
unsigned long lastFrameFetch = 0;
const unsigned long FRAME_RETRY_MS = 250;
uint8_t frameBitmap[1024];
unsigned long lastAnimationCheck = 0;

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
bool animationDrawn = false;  // a GIF frame is on screen, so the band may dance
int dancePhase = 0;
int scrollOffset = 0;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String currentText = "";
String currentVersion = "";
bool serverOnline = false;
unsigned long lastPoll = 0;

bool checkAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Animation check failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  JSONVar info = JSON.parse(http.getString());
  http.end();

  if (JSON.typeof(info) == "undefined") {
    Serial.println("Failed to parse animation info");
    return false;
  }

  int newVersion = (int)info["version"];
  int newFrameCount = (int)info["frame_count"];
  int newFrameMs = (int)info["frame_ms"];

  Serial.printf("Animation check: version=%d, frames=%d\n", newVersion, newFrameCount);

  if (newFrameMs > 0) animationFrameMs = newFrameMs;
  loadFrameDelays(info);

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    animationFrameCount = newFrameCount;
    animationFrameIndex = 0;
    restartAnimationSchedule();
    Serial.printf("Animation updated to version %d with %d frames\n", newVersion, newFrameCount);

    // Animation was cleared (e.g. a new message was sent): show the text again.
    if (newFrameCount == 0) drawScreen();
  }

  return true;
}

// Read the per-frame delays the server sends from the GIF's metadata.
void loadFrameDelays(JSONVar& info) {
  JSONVar delays = info["frame_delays"];
  int count = 0;

  if (JSON.typeof(delays) == "array") {
    count = delays.length();
    if (count > MAX_ANIMATION_FRAMES) count = MAX_ANIMATION_FRAMES;
  }

  for (int i = 0; i < MAX_ANIMATION_FRAMES; i++) {
    int ms = (i < count) ? (int)delays[i] : 0;
    frameDelays[i] = (ms > 0 && ms < 65535) ? ms : 0;
  }
}

unsigned long delayForFrame(int index) {
  if (index >= 0 && index < MAX_ANIMATION_FRAMES && frameDelays[index] > 0) {
    return frameDelays[index];
  }
  return animationFrameMs;
}

// Set the deadline for the frame after the one drawn at "now". The deadline
// advances by exactly the frame's delay so timing doesn't drift; if we have
// fallen a whole frame behind (slow network), restart the schedule instead.
void scheduleNextFrame(unsigned long now, int drawnIndex) {
  unsigned long d = delayForFrame(drawnIndex);

  if (animationResync || (long)(now - nextFrameAt) >= (long)d) {
    nextFrameAt = now + d;
  } else {
    nextFrameAt += d;
  }

  animationResync = false;
}

// Throw away any prefetched frame and start the schedule over.
void restartAnimationSchedule() {
  frameReady = false;
  animationResync = true;
  nextFrameAt = millis();
}

// Download a frame into frameBitmap without drawing it.
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
  Serial.printf("Displaying frame %d\n", index);
  display.clearDisplay();
  display.drawBitmap(0, 0, frameBitmap, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  drawDancingText();
  display.display();
  lastDanceTick = millis();
  animationDrawn = true;
}

// Keep the text moving between GIF frames (GIFs can have long delays).
void danceTick() {
  if (millis() - lastDanceTick < DANCE_INTERVAL_MS) return;
  drawDancingText();
  display.display();
  lastDanceTick = millis();
}

void drawScreen() {
  animationDrawn = false;
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

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverOnline = false;
    drawScreen();
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP + ":5050/api/message?device=" + DEVICE_NAME + "&screen=ssd1306";

  if (!http.begin(client, url)) {
    serverOnline = false;
    drawScreen();
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
        drawScreen();
      }
    } else {
      serverOnline = false;
      drawScreen();
    }
  } else {
    serverOnline = false;
    drawScreen();
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

  if (animationFrameCount > 0) {
    // Prefetch the next frame as soon as the previous one is on screen.
    if (!frameReady && millis() - lastFrameFetch >= FRAME_RETRY_MS) {
      lastFrameFetch = millis();
      frameReady = fetchAnimationFrame(animationFrameIndex);
    }

    if (frameReady && (long)(millis() - nextFrameAt) >= 0) {
      unsigned long now = millis();
      drawAnimationFrame(animationFrameIndex);
      scheduleNextFrame(now, animationFrameIndex);
      animationFrameIndex = (animationFrameIndex + 1) % animationFrameCount;
      frameReady = false;
      lastFrameFetch = 0;
    }

    // Only once a frame is on screen; otherwise the band would dance over stale content.
    if (animationDrawn) danceTick();
  }

  delay(10);
}
