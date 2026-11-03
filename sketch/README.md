# ESP32 Sketch

This folder contains the Arduino sketch for the ESP32 that connects to the Flask server and displays messages/animations on the OLED.

## Required Libraries

Install these libraries via the Arduino IDE Library Manager:

- **WiFi** (builtin with ESP32 board support)
- **Adafruit GFX Library** by Adafruit
- **Adafruit SSD1306** by Adafruit
- **ArduinoJson** by Benoit Blanchon

## Configuration

### Method 1: Direct Editing (Simpler)

Edit `esp-ssd1306.ino` and update these constants at the top:

```cpp
const char* WIFI_SSID = "YourNetworkName";
const char* WIFI_PASSWORD = "YourPassword";
const char* SERVER_IP = "192.168.1.183";  // Your Mac's IP
```

### Method 2: Secrets File (Recommended)

1. Copy `secrets.h.example` to `secrets.h`
2. Fill in your credentials
3. Add `#include "secrets.h"` at the top of `esp-ssd1306.ino`
4. Remove or comment out the hardcoded credentials

## Hardware Connections

- **OLED SDA** → GPIO 21
- **OLED SCL** → GPIO 22
- **OLED VCC** → 3.3V
- **OLED GND** → GND

## Board Selection

- Board: Elegoo ESP32 (ESP-EL-SM-012) or equivalent
- Upload Speed: 921600 (default) or 115200 if you encounter issues

## Serial Monitor

- Baud rate: 115200
- Use for debugging connection issues and viewing the ESP32's IP address

## Sketch Behavior

1. Connects to Wi-Fi on startup
2. Polls `/api/message` every 2 seconds for text updates
3. Polls `/api/animation` every 2 seconds for animation updates
4. Displays animation frames when available (priority over text)
5. Shows server connection status on the OLED
