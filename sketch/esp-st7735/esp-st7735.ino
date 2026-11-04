#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino_JSON.h>
#include <secrets.h>

// Display pins
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 23
#define TFT_SCLK 18

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

// Maximum RGB332 frame capacity: 144 * 192 pixels.
// The frame itself may have different dimensions, provided its pixel count
// fits this buffer and its position/dimensions fit the display.
#define MAX_FRAME_PIXELS (144 * 192)

// RGB565 conversion buffer. This is wide enough for any strip on the screen.
#define STRIP_ROWS 16
#define MAX_STRIP_PIXELS (SCREEN_WIDTH * STRIP_ROWS)

const unsigned long POLL_INTERVAL_MS = 2000;

Adafruit_ST7789 display(TFT_CS, TFT_DC, TFT_RST);

// Entire downloaded frame is stored as RGB332, one byte per pixel.
uint8_t frameBuffer[MAX_FRAME_PIXELS];

// Reusable buffer for converting and drawing a strip as RGB565.
uint16_t pixelBuffer[MAX_STRIP_PIXELS];

String currentText = "";

bool serverOnline = false;
bool animationActive = false;

int animationVersion = -1;
int animationFrameCount = 0;
int animationFrameIndex = 0;

int frameWidth = 0;
int frameHeight = 0;
int frameX = 0;
int frameY = 0;

unsigned long animationFrameMs = 120;
unsigned long lastPoll = 0;

// Per-frame display time from the GIF. Falls back to animationFrameMs.
#define MAX_ANIMATION_FRAMES 60
uint16_t frameDelays[MAX_ANIMATION_FRAMES];
// The next frame is fetched right after the current one is drawn, then drawn
// at nextFrameAt. Network latency therefore only matters if a fetch takes
// longer than the frame's delay.
bool frameReady = false;         // frameBuffer holds frame animationFrameIndex
bool animationResync = true;     // next draw starts a fresh schedule
unsigned long nextFrameAt = 0;   // millis() deadline to draw the buffered frame
unsigned long lastFrameFetch = 0;
const unsigned long FRAME_RETRY_MS = 250;

void drawTextScreen() {
  if (animationActive) {
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

bool checkAnimation() {
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

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Animation check: HTTP %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JSONVar info = JSON.parse(response);

  if (JSON.typeof(info) == "undefined") {
    Serial.println("Animation check: invalid JSON");
    return false;
  }

  int newVersion = (int)info["version"];
  int newFrameCount = (int)info["frame_count"];
  int newWidth = (int)info["width"];
  int newHeight = (int)info["height"];
  int newX = (int)info["x"];
  int newY = (int)info["y"];
  int newFrameMs = (int)info["frame_ms"];

  if (newFrameCount < 0) {
    newFrameCount = 0;
  }

  size_t newPixelCount = (size_t)newWidth * (size_t)newHeight;

  // Validate the variable-sized frame against the pixel budget and display.
  // A cleared animation (no frames) reports a 0x0 size, which is valid.
  if (newFrameCount > 0 &&
      (newWidth < 1 || newHeight < 1 ||
       newPixelCount > MAX_FRAME_PIXELS ||
       newX < 0 || newY < 0 ||
       newX + newWidth > SCREEN_WIDTH ||
       newY + newHeight > SCREEN_HEIGHT)) {
    Serial.printf("Invalid frame geometry: %dx%d at %d,%d\n",
                  newWidth, newHeight, newX, newY);
    return false;
  }

  if (newFrameMs > 0) {
    animationFrameMs = (unsigned long)newFrameMs;
  }

  loadFrameDelays(info);

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    animationFrameIndex = 0;
    restartAnimationSchedule();

    // Clear the previous GIF or message before the new animation starts.
    display.fillScreen(ST77XX_BLACK);

    Serial.printf("Animation changed: version %d, %d frames, %dx%d\n",
                  newVersion, newFrameCount, newWidth, newHeight);
  }

  if (newFrameCount > 0) {
    frameWidth = newWidth;
    frameHeight = newHeight;
    frameX = newX;
    frameY = newY;
  }

  animationFrameCount = newFrameCount;
  animationActive = animationFrameCount > 0;

  if (!animationActive) {
    // Clears the old animation and displays the current message.
    drawTextScreen();
  }

  return true;
}

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

// Download a frame into frameBuffer without drawing it.
bool fetchFrame(int frameIndex) {
  if (WiFi.status() != WL_CONNECTED ||
      frameWidth < 1 || frameHeight < 1) {
    return false;
  }

  size_t pixelCount = (size_t)frameWidth * (size_t)frameHeight;

  if (pixelCount > MAX_FRAME_PIXELS) {
    Serial.println("Frame exceeds RGB332 buffer");
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

  if (!readExact(stream, frameBuffer, pixelCount)) {
    Serial.printf("Frame %d: failed reading full frame\n", frameIndex);
    http.end();
    return false;
  }

  http.end();
  return true;
}

// Convert the buffered frame and draw it in RGB565 strips.
bool drawFrame() {
  for (int row = 0; row < frameHeight; row += STRIP_ROWS) {
    int rows = STRIP_ROWS;

    if (row + rows > frameHeight) {
      rows = frameHeight - row;
    }

    size_t stripPixels = (size_t)frameWidth * (size_t)rows;

    if (stripPixels > MAX_STRIP_PIXELS) {
      Serial.println("Strip exceeds RGB565 buffer");
      return false;
    }

    size_t sourceOffset = (size_t)row * (size_t)frameWidth;

    for (size_t i = 0; i < stripPixels; i++) {
      uint8_t c = frameBuffer[sourceOffset + i];

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
    display.setAddrWindow(frameX, frameY + row, frameWidth, rows);
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
  // It does nothing while an animation is active.
  drawTextScreen();
}

void setup() {
  Serial.begin(115200);
  delay(500);

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

  checkAnimation();
  drawTextScreen();

  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    pollServer();
    checkAnimation();
  }

  if (animationActive && animationFrameCount > 0) {
    // Prefetch the next frame as soon as the previous one is on screen.
    if (!frameReady && now - lastFrameFetch >= FRAME_RETRY_MS) {
      lastFrameFetch = now;
      frameReady = fetchFrame(animationFrameIndex);
    }

    now = millis();
  }

  if (animationActive &&
      animationFrameCount > 0 &&
      frameReady &&
      (long)(now - nextFrameAt) >= 0) {
    bool drawn = drawFrame();
    frameReady = false;
    lastFrameFetch = 0;

    if (drawn) {
      scheduleNextFrame(now, animationFrameIndex);
      animationFrameIndex++;

      if (animationFrameIndex >= animationFrameCount) {
        animationFrameIndex = 0;
      }
    }
  }

  delay(2);
}
