#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <secrets.h>

#define MATRIX_HARDWARE MD_MAX72XX::FC16_HW
#define MATRIX_DEVICES 4
#define MATRIX_CS 5

#define SOURCE_WIDTH 128
#define SOURCE_HEIGHT 64
#define SOURCE_BYTES 1024

const unsigned long ANIMATION_CHECK_MS = 2000;

MD_MAX72XX matrix(MATRIX_HARDWARE, MATRIX_CS, MATRIX_DEVICES);

int animationVersion = -1;
int animationFrameCount = 0;
int animationFrameIndex = 0;
unsigned long animationFrameMs = 120;
unsigned long lastAnimationFrame = 0;
unsigned long lastAnimationCheck = 0;

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

bool checkAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation?device=" + DEVICE_NAME;

  if (!http.begin(client, url)) return false;
  http.setTimeout(3000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  JSONVar info = JSON.parse(http.getString());
  http.end();

  if (JSON.typeof(info) == "undefined") return false;

  int newVersion = (int)info["version"];
  int newFrameCount = (int)info["frame_count"];
  int newFrameMs = (int)info["frame_ms"];

  if (newFrameMs > 0) animationFrameMs = newFrameMs;

  if (newVersion != animationVersion) {
    animationVersion = newVersion;
    animationFrameCount = newFrameCount;
    animationFrameIndex = 0;
  }

  return true;
}

bool fetchAndShowFrame(int index) {
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

  uint8_t bitmap[SOURCE_BYTES];
  WiFiClient* stream = http.getStreamPtr();
  size_t received = stream->readBytes(bitmap, sizeof(bitmap));
  http.end();

  if (received != sizeof(bitmap)) return false;

  showMatrixFrame(bitmap);
  return true;
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

  if (animationFrameCount > 0 &&
      now - lastAnimationFrame >= animationFrameMs) {
    lastAnimationFrame = now;

    if (fetchAndShowFrame(animationFrameIndex)) {
      animationFrameIndex =
          (animationFrameIndex + 1) % animationFrameCount;
    }
  }

  delay(10);
}
