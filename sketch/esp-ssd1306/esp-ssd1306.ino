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
unsigned long lastAnimationFrame = 0;
unsigned long lastAnimationCheck = 0;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String currentText = "";
String currentVersion = "";
bool serverOnline = false;
unsigned long lastPoll = 0;

bool checkAnimation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation";

  if (!http.begin(client, url)) return false;

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

bool showAnimationFrame(int index) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":5050/api/animation/frame/" + String(index);

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() != 1024) {
    http.end();
    return false;
  }

  uint8_t bitmap[1024];
  WiFiClient* stream = http.getStreamPtr();
  size_t received = stream->readBytes(bitmap, sizeof(bitmap));
  http.end();

  if (received != sizeof(bitmap)) return false;

  display.clearDisplay();
  display.drawBitmap(0, 0, bitmap, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
  return true;
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
  display.println("Message display");
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

  String url = String("http://") + SERVER_IP + ":5050/api/message?device_id=esp32-demo";

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

  if (animationFrameCount > 0 && millis() - lastAnimationFrame >= animationFrameMs) {
    lastAnimationFrame = millis();

    if (showAnimationFrame(animationFrameIndex)) {
      animationFrameIndex = (animationFrameIndex + 1) % animationFrameCount;
    }
  }

  delay(10);
}
