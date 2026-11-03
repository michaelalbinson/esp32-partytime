#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino_JSON.h>
#include <secrets.h>

// Your original working ESP32/display pins
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 23
#define TFT_SCLK 18

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

// Server supplies 128x64 RGB565 frames, high byte first
#define FRAME_WIDTH  128
#define FRAME_HEIGHT 64
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_BYTES  (FRAME_PIXELS * 2)

#define FRAME_X ((SCREEN_WIDTH - FRAME_WIDTH) / 2)
#define FRAME_Y ((SCREEN_HEIGHT - FRAME_HEIGHT) / 2)

#define BUFFER_COUNT 3

const unsigned long POLL_INTERVAL_MS = 2000;

Adafruit_ST7789 display(TFT_CS, TFT_DC, TFT_RST);

// Three 128x64 RGB565 buffers: about 48 KB total
uint16_t frameBuffers[BUFFER_COUNT][FRAME_PIXELS];

QueueHandle_t freeBufferQueue;
QueueHandle_t readyBufferQueue;
SemaphoreHandle_t displayMutex;

volatile int animationVersion = -1;
volatile int animationFrameCount = 0;
volatile unsigned long animationFrameMs = 120;

String currentText = "";
String currentVersion = "";
bool serverOnline = false;
bool animationStarted = false;

unsigned long lastPoll = 0;
unsigned long lastAnimationFrame = 0;

bool fetchColorFrame(int index, uint16_t* buffer) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP +
               ":5050/api/animation/rgb565/frame/" + String(index);

  http.setTimeout(5000);

  if (!http.begin(client, url)) return false;

  int code = http.GET();

  if (code != HTTP_CODE_OK || http.getSize() != FRAME_BYTES) {
    Serial.printf("Frame %d request failed; HTTP code %d, size %d\n",
                  index, code, http.getSize());
    http.end();
    return false;
  }

  uint8_t* raw = reinterpret_cast<uint8_t*>(buffer);
  WiFiClient* stream = http.getStreamPtr();
  size_t received = stream->readBytes(raw, FRAME_BYTES);

  http.end();

  if (received != FRAME_BYTES) {
    Serial.printf("Short frame read: %u of %d bytes\n",
                  (unsigned int)received, FRAME_BYTES);
    return false;
  }

  // Convert server's big-endian RGB565 bytes to native uint16_t values.
  for (int i = 0; i < FRAME_PIXELS; i++) {
    buffer[i] = (uint16_t(raw[i * 2]) << 8) | raw[i * 2 + 1];
  }

  return true;
}

void animationFetcherTask(void* parameter) {
  uint8_t slot = 0;
  int nextFrame = 0;
  int seenVersion = -1;

  while (true) {
    if (animationFrameCount <= 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (xQueueReceive(freeBufferQueue, &slot, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    int currentVersionValue = animationVersion;
    int currentFrameCount = animationFrameCount;

    if (currentVersionValue != seenVersion) {
      seenVersion = currentVersionValue;
      nextFrame = 0;
    }

    if (currentFrameCount <= 0) {
      xQueueSend(freeBufferQueue, &slot, portMAX_DELAY);
      continue;
    }

    if (nextFrame >= currentFrameCount) {
      nextFrame = 0;
    }

    if (fetchColorFrame(nextFrame, frameBuffers[slot])) {
      if (xQueueSend(readyBufferQueue, &slot, portMAX_DELAY) == pdTRUE) {
        nextFrame = (nextFrame + 1) % currentFrameCount;
      } else {
        xQueueSend(freeBufferQueue, &slot, portMAX_DELAY);
      }
    } else {
      xQueueSend(freeBufferQueue, &slot, portMAX_DELAY);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

bool checkAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP + ":5050/api/animation/rgb565";
  http.setTimeout(5000);

  if (!http.begin(client, url)) return false;

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  JSONVar info = JSON.parse(http.getString());
  http.end();

  if (JSON.typeof(info) == "undefined") {
    Serial.println("Could not parse animation info");
    return false;
  }

  int newVersion = (int)info["version"];
  int newFrameCount = (int)info["frame_count"];
  int newFrameMs = (int)info["frame_ms"];
  int newFrameBytes = (int)info["frame_bytes"];

  if (newFrameBytes != FRAME_BYTES) {
    Serial.printf("Unexpected frame size: %d bytes; expected %d\n",
                  newFrameBytes, FRAME_BYTES);
    return false;
  }

  if (newFrameMs > 0) {
    animationFrameMs = (unsigned long)newFrameMs;
  }

  animationFrameCount = newFrameCount;

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    Serial.printf("Animation updated: %d frames\n", newFrameCount);
  }

  return true;
}

void drawScreen() {
  if (xSemaphoreTake(displayMutex, portMAX_DELAY) != pdTRUE) return;

  // Once animation begins, don't redraw message/status over it.
  if (animationStarted) {
    xSemaphoreGive(displayMutex);
    return;
  }

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

  xSemaphoreGive(displayMutex);
}

void showStartup() {
  display.fillScreen(ST77XX_BLACK);
  display.setTextColor(ST77XX_WHITE);
  display.setTextSize(2);
  display.setTextWrap(true);
  display.setCursor(8, 12);
  display.println("Message display");
  display.println();
  display.println("Connecting to Wi-Fi...");
}

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) {
    serverOnline = false;
    drawScreen();
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + SERVER_IP +
               ":5050/api/message?device_id=esp32-demo";

  http.setTimeout(5000);

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

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  display.init(SCREEN_WIDTH, SCREEN_HEIGHT);
  display.setRotation(0);
  showStartup();

  displayMutex = xSemaphoreCreateMutex();
  freeBufferQueue = xQueueCreate(BUFFER_COUNT, sizeof(uint8_t));
  readyBufferQueue = xQueueCreate(BUFFER_COUNT, sizeof(uint8_t));

  if (displayMutex == nullptr ||
      freeBufferQueue == nullptr ||
      readyBufferQueue == nullptr) {
    Serial.println("Could not create display/buffer queues");
    while (true) delay(1000);
  }

  for (uint8_t i = 0; i < BUFFER_COUNT; i++) {
    xQueueSend(freeBufferQueue, &i, portMAX_DELAY);
  }

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

  checkAnimation();

  BaseType_t taskCreated = xTaskCreate(
    animationFetcherTask,
    "AnimationFetcher",
    8192,
    nullptr,
    1,
    nullptr
  );

  if (taskCreated != pdPASS) {
    Serial.println("Could not create animation fetch task");
  }
}

void loop() {
  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollServer();
    checkAnimation();
  }

  if (millis() - lastAnimationFrame >= animationFrameMs) {
    uint8_t slot;

    if (xQueueReceive(readyBufferQueue, &slot, 0) == pdTRUE) {
      if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        // Clear the status/message screen once, on the first GIF frame.
        if (!animationStarted) {
          display.fillScreen(ST77XX_BLACK);
          animationStarted = true;
        }

        display.drawRGBBitmap(
          FRAME_X,
          FRAME_Y,
          frameBuffers[slot],
          FRAME_WIDTH,
          FRAME_HEIGHT
        );

        xSemaphoreGive(displayMutex);
      }

      xQueueSend(freeBufferQueue, &slot, portMAX_DELAY);
      lastAnimationFrame = millis();
    }
  }

  delay(2);
}
